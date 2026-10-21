# Productor-consumidor en el servidor TCP

## Arquitectura actual

```text
Hilo principal: poll -> accept -> cola acotada de sockets
                                      |
                        mutex + condition_variable
                                      |
                     workers fijos -> handle_client
```

El productor es accept_loop(). Los consumidores son los worker_loop().
El trabajo encolado es una conexion TCP completa, no un comando individual.
Por defecto hay 4 trabajadores y espacio para 16 conexiones pendientes.

## Donde estan el mutex y la condicion

clients_mutex_ protege pending_clients_, active_clients_ y stopping_.
Modificar una deque desde varios hilos no es seguro sin coordinacion.
La misma exclusion permite pasar un socket de pendiente a activo sin que stop()
lo pierda de vista. Los contadores ServerStats siguen usando atomic.

El productor inserta con lock_guard y llama a notify_one(). El consumidor usa:

```cpp
std::unique_lock<std::mutex> lock(clients_mutex_);
clients_ready_.wait(lock, [this] {
    return stopping_ || !pending_clients_.empty();
});
```

wait libera el mutex al dormir y lo adquiere al despertar. El predicado evita
continuar con una cola vacia tras un despertar espurio y permite salir al parar.
unique_lock es necesario porque wait debe liberar y recuperar el bloqueo.
Para secciones sin espera basta lock_guard.

El trabajador extrae un socket y libera el mutex ANTES de llamar handle_client.
Si mantuviera el bloqueo durante recv, impediria al productor y a los demas
trabajadores acceder a la cola mientras espera a un cliente.

## Propiedad y apagado

El productor cierra sockets rechazados; stop() cierra los pendientes descartados;
cada trabajador cierra su socket activo al terminar. stop() usa shutdown sobre
los activos para desbloquear recv/send, pero deja su close al trabajador.
El registro y el cierre se coordinan con el mismo mutex para evitar que un
descriptor cerrado se reutilice y stop() actue sobre otra conexion.

El manejador de SIGINT/SIGTERM solo escribe un atomic<bool> lock-free. No puede
usar mutex ni condition_variable. El productor consulta la bandera mediante
poll con timeout de 100 ms y solicita la parada desde codigo normal.

1. stop() marca stopping_ bajo el mutex y descarta conexiones pendientes.
2. shutdown despierta la E/S de los clientes activos.
3. notify_all despierta a los trabajadores esperando en la condicion.
4. run() hace join de todos los trabajadores SIN mantener el mutex.
5. El hilo principal cierra el socket de escucha y retorna.

No se usa detach. stop() solicita la parada; run() espera la terminacion.
run() solo puede invocarse una vez. Si lo ejecutas desde otro hilo, debes
esperar a ese hilo antes de destruir el objeto TcpServer.
La parada interrumpe conexiones: no garantiza entregar respuestas pendientes.

## Demo visible de la cola

```bash
make
./build/tcp_server --workers 1 --queue-capacity 1
```

Abre tres terminales con `nc 127.0.0.1 9000`, en este orden:

1. A recibe el saludo. Dejalo conectado sin enviar comandos: ocupa el worker.
2. B conecta, pero no recibe saludo: su socket esta en la cola.
3. C se desconecta: se rechaza porque no queda sitio en la cola.

En A escribe QUIT. A recibe BYE y B recibe el saludo: el mismo trabajador ha
recogido otra conexion. En B prueba PING. Ctrl+C en el servidor debe terminar
incluso si B se queda esperando. Con --workers 2, dos clientes pueden ser
atendidos concurrentemente.

## Limites y estadisticas

- La cola es FIFO, pero con varios trabajadores el orden de finalizacion varia.
- Un cliente lento ocupa un trabajador. El pool limita recursos, no hace la E/S
  asincrona. Todavia no hay timeout por cliente.
- Una cola llena rechaza la conexion cerrandola, sin respuesta de protocolo.
- El backlog TCP del kernel es distinto de nuestra cola de aplicacion.
- STATS cuenta conexiones que empiezan a ser atendidas. No incluye pendientes
  ni rechazadas; active significa atendidas, no todas las TCP conectadas.
- Las lecturas de varios contadores atomicos no forman una instantanea unica.
- El PowerPoint se ha actualizado al pool actual, conservando los cambios del
  usuario y una copia de la version anterior.

## Verificacion

```bash
make test
make test-server
```

Los tests de integracion requieren Python 3. Comprueban cola llena, reutilizacion
de trabajadores, dos clientes simultaneos, desconexion abrupta, y apagado con
trabajadores ociosos y con conexiones activas/pendientes. CTest los incorpora
tambien, por lo que el workflow de Actions los ejecutara al publicar los cambios.
