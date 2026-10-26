# Criptografia: mTLS y PKCS#11

## Que hace cada pieza

TLS protege la confidencialidad e integridad del transporte. mTLS exige que
ambos extremos presenten certificados: el cliente verifica al servidor y el
servidor verifica al cliente. Se permiten TLS 1.2 y TLS 1.3, no versiones anteriores.

PKCS#11 permite usar la clave privada del servidor mediante un token. OpenSSL
delega la firma del handshake en su provider PKCS#11; el provider llama al modulo
SoftHSM. La clave se genera en el token como sensible y no exportable. No es una
clave PEM importada ni una implementacion propia de algoritmos criptograficos.

SoftHSM es software: NO ofrece el aislamiento fisico de un HSM. El administrador
del equipo puede acceder al almacenamiento y memoria del proceso. La CA y las
claves de cliente de esta demo permanecen en archivos; solamente la clave del
servidor del segundo modo reside en el token.

## 1. mTLS con archivos

Desde la raiz del proyecto, en macOS Apple Silicon:

```bash
brew install openssl@3
make
export OPENSSL=/opt/homebrew/opt/openssl@3/bin/openssl
python3 scripts/create_test_pki.py
./build/tcp_server --tls-cert .test-pki/server.crt \
  --tls-key .test-pki/server.key --tls-ca .test-pki/ca.crt
```

En otra terminal:

```bash
/opt/homebrew/opt/openssl@3/bin/openssl s_client \
  -connect 127.0.0.1:9000 -servername localhost -verify_hostname localhost \
  -verify_return_error -CAfile .test-pki/ca.crt \
  -cert .test-pki/client.crt -key .test-pki/client.key -quiet
```

Escribe PING, STATS o QUIT y pulsa Enter. `nc` no es cliente TLS y no recibira
el saludo. Omitir el certificado de cliente provoca rechazo, no un downgrade.
`-verify_return_error` y `-verify_hostname` son importantes: no basta con mostrar
un certificado ni con comprobar que fue firmado por una CA conocida.

Los certificados de hoja duran un dia; la CA dura dos. El generador se niega a
sobrescribir una PKI existente. Para renovarla en el laboratorio, aparta los
directorios `.test-pki` y `.softhsm-demo` y repite el aprovisionamiento completo.
Nunca uses estos certificados, CA ni PIN de laboratorio en produccion.

## 2. Clave del servidor en SoftHSM

Dependencias instaladas en este Mac: SoftHSM, OpenSC (pkcs11-tool), Meson,
Ninja y pkgconf. El provider v1.1.0 se compilo con OpenSSL 3.6.3, sin instalarlo
globalmente. Para reproducir la compilacion en un directorio temporal nuevo:

```bash
brew install softhsm opensc meson ninja pkgconf
git clone --depth 1 --branch v1.1.0 \
  https://github.com/openssl-projects/pkcs11-provider.git /tmp/tcp-pkcs11-provider
PKG_CONFIG_PATH=/opt/homebrew/opt/openssl@3/lib/pkgconfig \
  meson setup /tmp/tcp-pkcs11-provider/builddir /tmp/tcp-pkcs11-provider
meson compile -C /tmp/tcp-pkcs11-provider/builddir
OPENSSL=/opt/homebrew/opt/openssl@3/bin/openssl \
  python3 scripts/setup_softhsm.py /tmp/tcp-pkcs11-provider/builddir/src/pkcs11.dylib
```

El aprovisionamiento YA se ha ejecutado en este workspace. No lo repitas sobre
el mismo directorio. Crea un almacenamiento aislado y no modifica tokens ajenos.
El script copia el provider a `.softhsm-demo`, genera una clave RSA de 2048 bits
en el token y solicita un certificado firmado por la CA de laboratorio.
En Linux cambia SOFTHSM_MODULE y la ruta al provider (.so); compila el provider
contra la misma instalacion OpenSSL que usa el servidor.

Para arrancar (sin exportar configuracion global a toda la terminal):

```bash
SOFTHSM2_CONF="$PWD/.softhsm-demo/softhsm.conf" \
OPENSSL_CONF="$PWD/.softhsm-demo/openssl.cnf" \
./build/tcp_server --tls-cert .softhsm-demo/server.crt \
  --tls-key 'pkcs11:token=tcp-demo;object=server;type=private' \
  --tls-ca .test-pki/ca.crt
```

Conecta con el mismo cliente del apartado anterior. El PIN se lee desde un
archivo privado (0600) dentro de un directorio 0700; no va en argumentos ni
en historial. El PIN administrativo de este token desechable no se conserva.
El aprovisionamiento pasa PINs a OpenSC mediante variables de entorno; no es un
gestor de secretos de produccion. No publiques los directorios generados: ambos
estan ignorados por Git. El provider tiene acceso al PIN y abre sus sesiones.

## Donde mirar el codigo

- `ServerConfig`: rutas TLS y URI de clave.
- Constructor de `TcpServer`: crea SSL_CTX, carga CA/certificado y exige cliente.
- `OSSL_STORE_open`: abre archivo o URI; `SSL_CTX_use_PrivateKey` usa la clave.
- `tls_handshake`: SSL_accept antes de enviar cualquier dato del protocolo.
- `tls_retry`: WANT_READ/WANT_WRITE se traducen a poll con plazo absoluto.
- `read_line` y `send_all`: SSL_read_ex/SSL_write_ex en modo TLS.
- `tls_close`: envia close_notify sin esperar indefinidamente al cierre remoto.
- unique_ptr con deleter: libera SSL, SSL_CTX y objetos OSSL_STORE (RAII).

SSL_CTX se configura antes de lanzar workers y luego se comparte sin modificar.
Cada worker tiene su propio SSL, nunca compartido entre hilos. El provider
gestiona las sesiones del token. No se mantiene el mutex de cola al hacer E/S
ni durante operaciones criptograficas. El apagado fuerza shutdown del socket;
no garantiza close_notify en cierres por error o parada forzada.

## Pruebas

```bash
make test test-server test-tls smoke
make test-pkcs11
```

Las pruebas TLS generan una PKI temporal y cubren cliente valido, certificado
ausente, CA no confiable, cliente caducado, hostname incorrecto, clave que no
corresponde al certificado, timeout de handshake y apagado. La prueba PKCS#11
requiere el token aprovisionado: ejecuta 16 sesiones con hasta ocho clientes
simultaneos y comprueba que una URI inexistente impide arrancar.

CMake/CTest y GitHub Actions incluyen la suite TLS; PKCS#11 es opt-in local,
sin enviar claves ni PINs a GitHub. Valgrind incorpora la dependencia OpenSSL,
pero su demo de servidor sigue siendo TCP sin TLS por defecto.

## Limites honestos

- Sin opciones TLS sigue existiendo el modo didactico en claro, con advertencia.
- Con opciones TLS incompletas el servidor falla; no vuelve a texto claro.
- Todos los clientes firmados por la CA configurada y validos para clientAuth
  tienen los mismos permisos. No hay ACL por identidad, revocacion CRL/OCSP ni
  rotacion automatica de certificados o secretos.
- Los plazos limitan esperas de red, no una llamada bloqueada dentro de un driver
  HSM. Esa situacion requiere politica de timeouts/aislamiento del proveedor.
- El pool por conexion sigue teniendo sus limites de capacidad y equidad.
- Esto es una demo integrada y probada, no un producto certificado o auditado.

Referencias oficiales: [E/S TLS no bloqueante](https://docs.openssl.org/3.6/man3/SSL_get_error/),
[provider PKCS#11](https://github.com/openssl-projects/pkcs11-provider/blob/v1.1.0/HOWTO.md)
y [compilacion del provider](https://github.com/openssl-projects/pkcs11-provider/blob/v1.1.0/BUILD.md).
