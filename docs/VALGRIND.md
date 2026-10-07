# Practica de Valgrind

Valgrind ejecuta tu programa e instrumenta sus accesos a memoria. Memcheck
detecta accesos invalidos, usos de valores sin inicializar y fugas de heap.
Solo comprueba los caminos que ejecutas; un informe limpio no prueba que
todo el programa este libre de errores. Tampoco comprueba carreras entre hilos:
para eso existen Helgrind y ThreadSanitizer.

## Entorno

En este Mac ARM usamos Linux ARM dentro de Docker. No se usan los binarios
macOS de build/: el script compila de nuevo dentro del contenedor en /tmp.
El proyecto se monta como solo lectura. La primera ejecucion descarga una
imagen e instala g++, los simbolos de libc y Valgrind; las siguientes usan cache.

Desde la raiz del proyecto, con Docker Desktop arrancado:

```bash
make valgrind
```

Esto ejecuta los tests existentes del protocolo bajo Memcheck, incluidos JSON,
PING, ECHO y QUIT. Un error detectado devuelve codigo 99 (Make reporta su propio
codigo de fallo). Los tests tambien pueden fallar por sus assertions.

## Aprender a leer un error

```bash
bash scripts/valgrind.sh bug
bash scripts/valgrind.sh fixed
```

El primer comando falla intencionadamente: examples/valgrind_memory.cpp lee un
int despues de delete. Busca `Invalid read of size 4`: la primera traza indica
el acceso incorrecto; las siguientes muestran donde se libero y reservo el bloque.
El valor impreso no es fiable porque hay comportamiento indefinido.
La version fixed usa unique_ptr y libera el recurso al terminar su ambito.

## Opciones del comando

El script compila con `-g -O0`: simbolos con archivo/linea y sin optimizacion,
para poder seguir mejor las trazas. `-pthread` habilita el soporte de hilos.

| Opcion | Para que sirve |
| --- | --- |
| --tool=memcheck | Seleccionar el analizador de memoria |
| --leak-check=full | Mostrar trazas de las fugas |
| --show-leak-kinds=all | Mostrar todas las categorias de memoria pendiente |
| --track-origins=yes | Rastrear el origen de valores sin inicializar |
| --track-fds=yes | Mostrar descriptores abiertos al terminar, incluidos sockets |
| --error-exitcode=99 | Devolver fallo si se detectan errores |

`definitely lost` significa que ya no existe un puntero al bloque reservado;
`indirectly lost`, que se pierde a traves de otro bloque perdido;
`possibly lost`, que Valgrind solo encuentra punteros interiores;
`still reachable`, que todavia hay un puntero al bloque. Esta ultima categoria
no equivale automaticamente a una fuga. Por defecto no todas las categorias
cuentan como errores para --error-exitcode: las fugas definite/possible si.

Busca `ERROR SUMMARY: 0 errors` y revisa tambien `HEAP SUMMARY` y los
descriptores abiertos. stdin/stdout/stderr suelen seguir abiertos normalmente.
Puede haber un use-after-free aunque no haya ninguna fuga, como en el ejemplo.

## Servidor TCP real

En una terminal interactiva:

```bash
make valgrind-server
```

En otra terminal del Mac:

```bash
printf 'PING\nECHO hola\n{"command":"PING"}\nQUIT\n' | nc 127.0.0.1 9000
```

Tras recibir BYE y cerrar los clientes, pulsa Ctrl+C en la terminal del servidor
para obtener el informe. El puerto se publica solo en localhost del Mac.
QUIT cierra una conexion, no el servidor entero.

Este servidor didactico usa hilos detached y no los espera al apagar.
Cerrar con clientes activos puede producir informes distintos y problemas de
vida util del objeto TcpServer. No atribuyas esos informes a fugas sin examinar
las trazas y el estado de los hilos. Para la primera practica, los tests son mas
faciles de interpretar porque terminan por si solos.

Referencia: https://valgrind.org/docs/manual/mc-manual.html
Plataformas: https://valgrind.org/info/platforms.html
