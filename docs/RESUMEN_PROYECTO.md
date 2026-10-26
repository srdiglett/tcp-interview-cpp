# Resumen del proyecto: servidor concurrente C++

## 1. Que es y para que sirve

Un proyecto personal en C++17 para practicar el desarrollo de un servicio de
red: concurrencia, control de recursos, protocolo, apagado, seguridad y pruebas.
Actualmente ofrece comandos de diagnostico por TCP, con modo seguro mTLS y
posibilidad de usar una clave privada mediante PKCS#11.

No es un servidor de criptografia ni un sistema de supervision terminado.
Supervisar dispositivos empotrados mediante heartbeats es una posible evolucion;
todavia no hay registro de dispositivos, estados online/offline ni persistencia.

**Presentacion en una frase:**

> He desarrollado un servidor concurrente en C++ con recursos acotados, apagado
> coordinado y transporte mTLS; he integrado PKCS#11 para delegar la firma del
> handshake en un token software, y he probado tanto el funcionamiento como
> los principales casos de error.

## 2. Arquitectura y archivos

```text
main.cpp: argumentos, senales y arranque
                  |
                  v
TcpServer: escucha -> cola acotada -> pool de workers
                                          |
                                  TCP o sesion mTLS
                                          |
                                  LineProtocol
                                          |
                                  respuesta + cierre
```

| Archivo | Responsabilidad |
| --- | --- |
| `src/main.cpp` | Valida argumentos, registra SIGINT/SIGTERM y ejecuta el servidor. |
| `include/tcp_interview/server.hpp` | Configuracion, miembros y contrato de TcpServer. |
| `src/server.cpp` | Sockets, cola, workers, plazos, TLS y carga de clave. |
| `src/protocol.cpp` | Interpreta comandos de texto o JSON y genera respuestas. |
| `include/tcp_interview/protocol.hpp` | LineProtocol, CommandResult y contadores atomicos. |
| `tests/` | Pruebas unitarias e integracion TCP, TLS y PKCS#11. |
| `scripts/` | Smoke test, Valgrind, QEMU y aprovisionamiento criptografico. |
| `examples/` | Ejemplos independientes: SFTP, analytics y errores de memoria. |

La logica de comandos no depende de OpenSSL ni del socket. Esto permite probarla
sin abrir conexiones y cambiar el transporte sin reescribir cada comando.

## 3. Recorrido de una conexion

1. `socket()` crea el descriptor; `bind()` lo asocia a una direccion y puerto.
2. `listen()` habilita la escucha; `poll()` espera disponibilidad sin espera activa.
3. `accept()` crea un socket nuevo para el cliente; la escucha sigue abierta.
4. El productor introduce la conexion en la cola. Si esta llena, la rechaza.
5. Un worker extrae la conexion y la registra como activa.
6. En modo TLS, completa y verifica el handshake antes de enviar el saludo.
7. Lee mensajes completos, ejecuta el protocolo y envia respuestas.
8. QUIT, desconexion, error o timeout terminan la atencion; el worker cierra el socket.

## 4. Concurrencia: pool, cola, mutex y atomics

Por defecto hay **4 workers y 16 plazas de conexiones pendientes**. Cada worker
atiende una conexion completa, no una peticion suelta. La cola es acotada porque
tiene capacidad maxima: no crece indefinidamente si llegan mas clientes.

El productor acepta conexiones; los consumidores son los workers. Una
`condition_variable` les permite dormir cuando no hay trabajo. Su `wait()`
libera el mutex mientras espera y vuelve a comprobar el predicado al despertar.

El mutex protege la cola, el estado de parada y el registro de conexiones activas.
No permanece adquirido durante la E/S ni durante las operaciones criptograficas.
Los contadores usan `std::atomic<int>`: sus incrementos individuales no necesitan
ese mutex, pero leer varios atomics no produce una instantanea conjunta coherente.

**Decision de arquitectura:** el pool acota hilos y permite hacer join; no implica
que sea mas rapido que otras opciones. Los sockets son no bloqueantes y utilizan
poll, pero cada worker sigue dedicado a una conexion. No es un event loop que
multiplexe todos los clientes. Un cliente activo puede retener su worker.

## 5. Protocolo, framing y plazos

TCP es un flujo de bytes: un recv no equivale a una peticion. El **framing**
define donde acaba cada mensaje: aqui, un salto de linea LF (`\n`).

| Comando | Respuesta |
| --- | --- |
| `PING` | `PONG` |
| `ECHO hola` | `hola` |
| `TIME` | Fecha y hora UTC |
| `STATS` | Conexiones totales, activas y comandos |
| `HELP` | Ayuda de comandos |
| `QUIT` | `BYE` y cierre |

Tambien se acepta `{"command":"ECHO","text":"hola"}` seguido de LF. Las
respuestas siguen siendo texto; no es una API de respuestas JSON. El parser
actual es didactico y limitado, no Boost.JSON.

- Se acumulan lecturas parciales y se separan mensajes recibidos juntos.
- El limite es 4096 bytes antes de LF. EOF sin LF no ejecuta un comando incompleto.
- Se completa el envio aunque una escritura solo transmita parte de la respuesta.
- Los campos con saltos de linea se rechazan para no introducir respuestas falsas.
- Hay un plazo absoluto por lectura de mensaje, escritura y handshake TLS.
- Por defecto son 10000 ms, medidos con `steady_clock`.
- Enviar bytes sueltos no renueva el plazo de un mensaje incompleto.

La cola no tiene timeout propio. Los plazos limitan esperas de red, no interrumpen
una llamada bloqueada dentro de un driver HSM. Tampoco garantizan equidad o
proteccion completa contra denegacion de servicio.

## 6. Ciclo de vida, recursos y apagado

El ciclo de vida abarca creacion, uso y liberacion de cada recurso, incluyendo
las rutas de error. El productor cierra conexiones rechazadas; stop cierra las
pendientes; el worker cierra su conexion activa.

SIGINT (por ejemplo Ctrl+C) y SIGTERM solicitan la parada. El manejador solo
activa un atomic lock-free. El codigo normal realiza el resto:

```text
solicitud -> stop -> cerrar pendientes y shutdown de activas
                 -> notify_all -> join de workers -> cerrar escucha
```

**No usar mutex en el manejador:** la senal podria interrumpir un hilo que ya lo
tiene adquirido. **No hacer join manteniendo el mutex:** el worker podria
necesitarlo para terminar. Ambos casos pueden provocar interbloqueo.

Los objetos OpenSSL usan RAII con `unique_ptr` y deleters: se liberan al salir de
su ambito, incluso por excepcion. Los sockets aun usan cierre explicito coordinado;
convertir su propiedad a RAII sigue pendiente. El cierre TLS normal envia
close_notify; una parada forzada no garantiza ese aviso ni completar respuestas.

## 7. Criptografia: TLS, mTLS y PKCS#11

| Pieza | Funcion en el proyecto |
| --- | --- |
| TLS | Cifra el transporte y protege su integridad; se admiten TLS 1.2 y 1.3. |
| mTLS | Ambos extremos presentan certificados y verifican al otro. |
| OpenSSL | Implementa TLS y las operaciones criptograficas. |
| CA de laboratorio | Firma certificados de servidor y cliente para la demo. |
| PKCS#11 | Interfaz para usar la clave de un token sin extraerla a la aplicacion. |
| Provider PKCS#11 | Conecta OpenSSL con el modulo del token. |
| SoftHSM | Token software usado para practicar sin hardware dedicado. |

```text
Cliente -- mTLS --> Servidor C++ / OpenSSL
                                |
                         Provider PKCS#11
                                |
                             SoftHSM
                      firma con clave privada
```

Hay dos modos seguros: clave del servidor en archivo PEM o clave identificada
por una URI `pkcs11:`. En el segundo, una clave RSA de 2048 bits se genera dentro
del token como sensible y no exportable; se utiliza para firmar el handshake.
El certificado contiene la clave publica, no la privada. TLS sigue gestionando
el canal: PKCS#11 no sustituye TLS ni cifra por si solo cada mensaje de la aplicacion.

El PIN se lee de un archivo privado y no se pone en los argumentos del servidor.
Los tokens, claves y certificados generados estan excluidos de Git. Las claves
de CA y cliente de la demo siguen en archivos. SoftHSM no proporciona aislamiento
fisico: no equivale a un HSM real frente al administrador de la maquina.

El cliente verifica CA y hostname; el servidor exige un certificado valido de
cliente. No hay permisos distintos por identidad: **autenticacion no equivale
a autorizacion**. Sin opciones TLS, el servidor sigue en claro con advertencia;
con configuracion TLS incompleta falla, no vuelve silenciosamente a TCP plano.

## 8. Metodos importantes para leer el codigo

| Metodo o funcion | Que hace |
| --- | --- |
| `TcpServer::TcpServer` | Valida configuracion y prepara SSL_CTX, certificados y clave. |
| `run` | Abre escucha, arranca workers y coordina la salida. |
| `accept_loop` | Espera conexiones, admite o rechaza y notifica a un worker. |
| `worker_loop` | Espera trabajo, extrae conexion, la atiende y la cierra. |
| `handle_client` | Crea sesion TLS si procede; enlaza lectura, protocolo y respuesta. |
| `wait_ready` | Espera disponibilidad con poll respetando un plazo. |
| `read_line` / `send_all` | Gestionan framing, lecturas y escrituras parciales. |
| `tls_handshake` | Ejecuta SSL_accept y resuelve esperas de red. |
| `tls_retry` | Traduce WANT_READ/WANT_WRITE a espera de lectura/escritura. |
| `tls_close` | Envia el aviso de cierre TLS con plazo. |
| `stop` / `join_workers` | Solicitan la parada y esperan sin mantener el mutex. |
| `LineProtocol::handle` | Distingue texto/JSON y despacha el comando. |
| `execute_command` | Genera la respuesta y decide si cerrar. |
| `OSSL_STORE_open` | API OpenSSL que carga clave desde archivo o provider/URI. |

SSL_CTX se configura antes de los hilos y luego se comparte sin modificar.
Cada worker posee su propio SSL; el provider gestiona las sesiones del token.

## 9. Compilar y ejecutar

Desde la raiz del repositorio, comandos para este Mac Apple Silicon:

```bash
make
./build/tcp_server --workers 4 --queue-capacity 16 --io-timeout-ms 10000
```

En otra terminal, para TCP sin cifrar:

```bash
nc 127.0.0.1 9000
```

Para mTLS con certificados y clave PEM ya preparados:

```bash
./build/tcp_server --tls-cert .test-pki/server.crt \
  --tls-key .test-pki/server.key --tls-ca .test-pki/ca.crt
```

Para mTLS con la clave del token ya aprovisionado:

```bash
SOFTHSM2_CONF="$PWD/.softhsm-demo/softhsm.conf" \
OPENSSL_CONF="$PWD/.softhsm-demo/openssl.cnf" \
./build/tcp_server --tls-cert .softhsm-demo/server.crt \
  --tls-key 'pkcs11:token=tcp-demo;object=server;type=private' \
  --tls-ca .test-pki/ca.crt
```

Cliente para cualquiera de los dos modos mTLS:

```bash
/opt/homebrew/opt/openssl@3/bin/openssl s_client \
  -connect 127.0.0.1:9000 -servername localhost -verify_hostname localhost \
  -verify_return_error -CAfile .test-pki/ca.crt \
  -cert .test-pki/client.crt -key .test-pki/client.key -quiet
```

No arranques los tres servidores a la vez en el mismo puerto. `nc` no sirve
como cliente TLS. Escribe PING y pulsa Enter; QUIT cierra la sesion y Ctrl+C
en la terminal del servidor solicita su parada.

**Antes de presentar:** comprobar certificados. Los de servidor/cliente duran
un dia y la CA dos. El aprovisionamiento inicial no garantiza que sigan validos.
Los scripts no sobrescriben directorios existentes. Instalacion, generacion y
renovacion de laboratorio: [CRYPTOGRAPHY.md](CRYPTOGRAPHY.md).

## 10. Pruebas, Valgrind y CI

```bash
make test test-server test-tls smoke
make test-pkcs11
```

La ultima verificacion local de la implementacion paso las unitarias, 12 pruebas
TCP, 10 TLS, la prueba PKCS#11 y el smoke test. No significa que se hayan repetido
al escribir este resumen. Las pruebas TLS generan su PKI temporal; PKCS#11
necesita el token local y certificados vigentes.

| Evidencia | Cobertura |
| --- | --- |
| Unitarias | Comandos y errores sin red. |
| TCP | Cola llena, reutilizacion, framing, timeouts, desconexion y senales. |
| TLS | Cliente valido; rechazo sin certificado, CA incorrecta, caducidad y hostname. Tambien clave incorrecta, plazos y parada. |
| PKCS#11 | 16 sesiones, hasta 8 clientes simultaneos; fallo si falta clave o provider. |
| Smoke | Recorrido sencillo desde un script de shell. |
| Valgrind | Analisis dinamico de memoria en Linux/Docker; no demuestra ausencia de carreras. |
| SonarQube | Analisis estatico y reglas de calidad; no reemplaza pruebas. |

GitHub Actions es la plataforma de automatizacion, comparable a Jenkins;
CI/CD es la practica. Aqui se automatizan build y tests con CMake/CTest, y un
job opcional de SonarQube. No hay despliegue continuo implementado.

El workflow incluye TLS; PKCS#11 es una prueba local opt-in sin secretos en CI.
La actualizacion criptografica se verifico con Make: su ejecucion remota en
GitHub y la nueva ruta bajo Valgrind no quedaron verificadas en esa entrega.

## 11. Ejemplos complementarios

**SFTP/SSH:** cliente C++ con libssh2 que sube un archivo; no implementa SSH desde
cero. Su huella de servidor se muestra pero no se valida contra known_hosts:
limitacion conocida, mantener la demo en localhost. Es independiente del mTLS
del servidor y su seguridad no mejora automaticamente al anadir TLS a este.

**Yocto/QEMU:** imagen precompilada oficial de Linux ARM64, con kernel propio;
no es un contenedor ni una distribucion Yocto construida por nosotros. OpenSSH
de la VM se publica en localhost:2222. Subir un archivo y comparar SHA-256
demuestra transferencia, no que nuestro servidor C++ se ejecute en ARM64.

**Analytics:** programa independiente con Boost.JSON que lee eventos JSON Lines,
agrupa metricas por celda y genera alertas de calidad. No esta conectado al
servidor TCP. **Valgrind:** ejemplo separado con errores intencionados de memoria.

```bash
make ux-analytics
./build/ux_analytics_pipeline < data/sample_network_events.jsonl
make sftp-example
bash scripts/yocto_qemu.sh start
```

La VM requiere preparacion previa: [YOCTO_QEMU.md](YOCTO_QEMU.md).

## 12. Pendientes y limites

- Seguridad: ACL por identidad, revocacion CRL/OCSP y rotacion de certificados/PIN.
- Codigo: RAII para sockets y sustitucion del parser JSON didactico.
- Operacion: logs estructurados, metricas, caducidad de cola y politica de sesiones.
- Evidencia: deteccion de carreras, cobertura, carga prolongada y pruebas con HSM real.
- Funcionalidad propuesta: heartbeats, estados de dispositivos y persistencia SQLite.
- Escalabilidad: evaluar un event loop segun medidas; no cambiar arquitectura por moda.

Es un laboratorio integrado, no un producto certificado, auditado o listo para
exponerse a Internet. Las pruebas concretas aportan evidencia, no garantias absolutas.

## 13. Guion de 15 minutos

| Tiempo | Tema |
| --- | --- |
| 0-2 min | Objetivo, alcance real y arquitectura. |
| 2-5 min | Pool, cola acotada, mutex y propiedad de conexiones. |
| 5-7 min | Framing, plazos y apagado coordinado. |
| 7-11 min | mTLS, PKCS#11, SoftHSM y limites de seguridad. |
| 11-13 min | Demo breve y pruebas positivas/negativas. |
| 13-15 min | CI, decisiones, pendientes y cierre. |

Para la demo, escoger un recorrido: PING/QUIT por mTLS con clave en el token,
o saturacion del pool con un worker y una plaza. No intentar ensenar todas las
herramientas y la VM en quince minutos. Mantener SFTP/Yocto como contexto opcional.

**Cierre:** el valor del proyecto no es responder PONG; es poder explicar como
controla recursos, verifica identidades y responde a desconexiones, saturacion
y parada, sin ocultar los limites que todavia tiene.

## Documentacion relacionada

- [Concurrencia y mutex](CONCURRENCY.md)
- [Criptografia y aprovisionamiento](CRYPTOGRAPHY.md)
- [Valgrind](VALGRIND.md)
- [CI y SonarQube](CI_SONARQUBE.md)
- [Yocto/QEMU](YOCTO_QEMU.md)
- [Notas de estudio](STUDY_NOTES.md)
- [Hoja de ruta](ROADMAP.md)
