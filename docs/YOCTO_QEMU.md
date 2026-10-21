# Dispositivo Linux simulado con Yocto y QEMU

Usamos una imagen oficial precompilada de Yocto 5.0.17 (Scarthgap), maquina
qemuarm64, core-image-full-cmdline. No hemos construido una distribucion propia
ni una BSP. QEMU emula una maquina ARM64 con su propio kernel Linux; no es un
contenedor. El servidor TCP y este servidor SFTP son programas independientes.

## Preparacion (una vez)

Desde la raiz del proyecto, en el Mac:

```bash
brew install qemu
bash scripts/yocto_qemu.sh prepare
make sftp-example
```

prepare descarga unos 407 MiB de kernel y disco y verifica SHA256 contra los
ficheros publicados por Yocto. Crea un disco qcow2 que guarda cambios sin modificar
el disco original y una clave SSH exclusiva de la demo, sin passphrase.
Todo se guarda en .yocto-qemu/, excluido de Git. No compartas esa clave privada.
La imagen de desarrollo puede contener claves de host preinstaladas y acceso
root sin password: solo se usa localmente, no debe exponerse a una red real.

## Arrancar

```bash
bash scripts/yocto_qemu.sh start
```

Se abre la consola serie de Linux. Usa root en el login. La VM usa 512 MiB de RAM,
dos CPU virtuales y emulacion TCG. Su puerto SSH 22 se publica solo en
127.0.0.1:2222 del Mac. No requiere Docker ni privilegios root del Mac.
No arranques dos instancias sobre el mismo disco.

## Autorizar la clave (primer arranque)

En este Mac la clave ya se ha autorizado durante la preparacion de la demo.
Estos pasos son para reproducirla con un disco nuevo.

En otra terminal del Mac:

```bash
bash scripts/yocto_qemu.sh key
```

En la consola de Yocto crea el directorio y pega la linea publica obtenida
en lugar de LA_CLAVE_PUBLICA (no la clave privada):

```sh
mkdir -p /home/root/.ssh /home/root/upload
chmod 700 /home/root/.ssh
printf '%s\n' 'LA_CLAVE_PUBLICA' > /home/root/.ssh/authorized_keys
chmod 600 /home/root/.ssh/authorized_keys
```

La configuracion inicial se conserva en device.qcow2. Para esta practica el
cliente se conecta como root; para un dispositivo real usariamos un usuario
restringido y eliminariamos el acceso root sin password.

## Subir un archivo desde nuestro cliente C++

En el Mac, con la VM arrancada:

```bash
./build/sftp_upload_libssh2 \
  127.0.0.1 root .yocto-qemu/id_ed25519 \
  data/sample_network_events.jsonl \
  /home/root/upload/sample_network_events.jsonl 2222
```

El ultimo argumento es el puerto opcional (22 si se omite).
El cliente sigue imprimiendo la huella SSH sin validarla contra known_hosts:
es una limitacion conocida; mantener esta prueba en localhost.

Verificar en la consola de Yocto:

```sh
cat /etc/issue
uname -m
sha256sum /home/root/upload/sample_network_events.jsonl
```

Y comparar con el Mac:

```bash
shasum -a 256 data/sample_network_events.jsonl
```

## Apagar y volver a arrancar

En la consola de Yocto: `poweroff`. Espera a que QEMU termine.
Para arrancar de nuevo usa el mismo comando start: los archivos persisten.
Como salida de emergencia, Ctrl+A seguido de X termina QEMU, pero puede perder
escrituras pendientes; preferir poweroff.

## Que demostrar

Mac / cliente C++ -> TCP localhost:2222 -> SSH/SFTP -> Linux ARM64 en QEMU.
Mostrar /etc/issue, subir el JSON y comparar hashes prueba la transferencia
real a otro sistema operativo. No demuestra ejecucion del servidor C++ en la VM:
eso requeriria compilarlo para Linux ARM64 con un toolchain compatible.

Origen oficial:
https://downloads.yoctoproject.org/releases/yocto/yocto-5.0.17/machines/qemu/qemuarm64/
