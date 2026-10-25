# Del ejercicio a un servicio real

## Para que puede servir

Un caso de uso propuesto es supervisar dispositivos Linux empotrados: cada
dispositivo envia periodicamente un heartbeat con su identificador y uptime;
el servidor mantiene su ultimo estado y permite detectar dispositivos que han
dejado de reportar. Es una propuesta de evolucion, no una funcionalidad actual.

Hoy tenemos la base de transporte y concurrencia. PING comprueba que el servicio
responde, pero NO registra dispositivos ni permite saber si uno esta offline.
El ejemplo de analytics y el cliente SFTP son programas separados del servidor.

## Implementado y comprobable

- Pool fijo de workers y cola acotada de conexiones.
- Mutex y condition_variable para transferir conexiones entre productor y consumidores.
- Rechazo de conexiones cuando la cola esta llena.
- Apagado con SIGINT/SIGTERM: cierre de pendientes, shutdown de activas y join.
- Contadores atomicos y comandos de diagnostico.
- Lecturas en bloques: un recv puede contener parte de un mensaje o varios.
- Limite de 4096 bytes antes del delimitador LF; EOF no sustituye al delimitador.
- Plazo absoluto por lectura de mensaje y por escritura de respuesta.
- Sockets no bloqueantes y poll para respetar esos plazos.
- Validacion estricta de puerto y timeout; proteccion del framing de respuestas.

## Probar los plazos

Desde la raiz del repositorio:

```bash
make test test-server
./build/tcp_server --workers 1 --queue-capacity 1 --io-timeout-ms 5000
```

Abre `nc 127.0.0.1 9000` en dos terminales. La primera recibe el saludo y
ocupa el unico worker; la segunda espera en cola. Sin escribir en la primera,
transcurridos unos cinco segundos se cierra y la segunda recibe el saludo.
Envia `PING` y pulsa Enter en la segunda para recibir `PONG`.

El timeout por defecto es 10000 ms. Se mide con steady_clock, que no depende
de ajustes del reloj civil. Cada nuevo mensaje procesado obtiene su propio
plazo; enviar bytes de un mensaje incompleto no lo renueva. La escritura de
cada respuesta tiene un plazo independiente con el mismo valor.

La espera en cola NO tiene timeout propio. Un cliente que siga enviando
mensajes completos a tiempo puede conservar un worker indefinidamente.
Estos plazos no garantizan equidad ni protegen por si solos contra un DoS.

## Pendiente, en orden propuesto

1. Sustituir el parser JSON didactico por Boost.JSON, definir esquema y version.
2. Implementar heartbeat, registro acotado y consulta de dispositivos online/offline.
3. Persistir estado e historial acotado en SQLite, con transacciones y tests de reinicio.
4. Crear un agente de dispositivo con reconexion y backoff; probarlo en Yocto/QEMU.
5. Definir autenticacion y transporte TLS antes de exponer el servicio a una red no fiable.
6. Expresar propiedad de sockets con RAII, manteniendo la coordinacion de shutdown/close.
7. Anadir logs estructurados, metricas de saturacion y tests de carga y sanitizadores.
8. Evaluar expiracion de cola y limite de duracion de sesion; cambiar a event loop
   si necesitamos muchas conexiones persistentes con pocos hilos.

No tiene sentido afirmar que es un producto terminado o listo para produccion:
no hay autenticacion, TLS ni persistencia en el servidor actual. Mantenerlo en
loopback para practicar; no abrir el puerto a Internet.

## Explicacion para entrevista

El productor acepta conexiones; la cola transfiere su propiedad a un worker.
El mutex protege la cola y el registro de conexiones activas, no toda la E/S.
Los workers esperan sin consumir CPU usando condition_variable. Una vez tienen
conexion, esperan disponibilidad de socket con poll. Esto sigue siendo un pool
por conexion, no un event loop que multiplexa todos los clientes.

La mejora de los plazos limita la ocupacion por clientes inactivos o mensajes
incompletos. Es una decision operativa con un coste: un cliente legitimo que
tarde mas que el plazo tambien se desconecta. Se debe configurar segun el uso.
