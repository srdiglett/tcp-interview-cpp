# GitHub Actions y SonarQube

## Que estamos montando

CI (integracion continua) es compilar y comprobar cada cambio automaticamente.
CD es entrega o despliegue continuo. GitHub Actions y Jenkins son herramientas
con las que implementar esos procesos; SonarQube analiza la calidad del codigo.
Aqui implementamos CI. No hay despliegue porque no hemos definido un servidor
de destino. Jenkins seria una alternativa a Actions, no un requisito adicional.

El workflow .github/workflows/ci.yml se ejecuta en Linux al hacer push, abrir o
actualizar una pull request, y manualmente desde Actions.

1. Checkout descarga el codigo.
2. CMake configura y compila el servidor y los tests en Debug.
3. CTest ejecuta protocol_tests y las pruebas de red del pool con Python 3.
   Debug conserva los assert de los tests C++.
4. En la rama principal, si SONAR_ENABLED=true, otro job analiza el servidor
   en SonarQube Cloud y espera el resultado del quality gate.

Un job es un grupo de pasos que comparte una maquina. `needs: build-test`
impide ejecutar Sonar si falla la compilacion o los tests. Cada job tiene su
propia maquina, por eso el job de Sonar vuelve a compilar.

## Activacion en GitHub y Cloud

Esta carpeta debe ser la raiz de su propio repositorio GitHub: .github/ debe
estar en la raiz, no dentro de una subcarpeta de quiniela o interviews.
Ya tiene su propio repositorio Git local. Para activar Actions debe publicarse
en GitHub; crear el repositorio local no lo publica automaticamente.

En SonarQube Cloud, inicia sesion con GitHub, importa ese repositorio y selecciona
analisis mediante GitHub Actions. Desactiva Automatic Analysis si esta activado,
para usar el analisis de CI. Esta configuracion utiliza la region EU
(https://sonarcloud.io); un proyecto en otra region necesita adaptar el endpoint
y los ajustes regionales del scanner segun la documentacion de Sonar.

Comprueba que el plan elegido permite analizar C++ y tu tipo de repositorio.
No hemos contratado ningun plan ni instalado SonarQube Server localmente.

En GitHub: Settings > Secrets and variables > Actions, configura:

| Tipo | Nombre | Valor |
| --- | --- | --- |
| Secret | SONAR_TOKEN | Token de analisis generado en SonarQube Cloud |
| Variable | SONAR_PROJECT_KEY | Clave exacta del proyecto en Sonar |
| Variable | SONAR_ORGANIZATION | Clave de la organizacion en Sonar |
| Variable | SONAR_ENABLED | true |

No pegues el token en el codigo ni en conversaciones. Las claves de proyecto y
organizacion no son el token. Con SONAR_ENABLED sin configurar, los tests se
ejecutan y el job de Sonar aparece como skipped. Con true, una configuracion
incompleta produce un fallo explicito, sin mostrar el secreto.

Haz push en la rama principal o usa Actions > C++ CI > Run workflow en esa rama.
La CI compila y prueba tambien las PR, pero Sonar se limita a la rama principal
para no asumir soporte de analisis de ramas o PR en tu plan.

## Como analiza C++

`CMAKE_EXPORT_COMPILE_COMMANDS=ON` genera compile_commands.json: una lista de
archivos, comandos de compilacion, includes y flags. No contiene resultados de
tests. Sonar usa esa informacion para entender el codigo en su contexto real.
Se genera y consume en el mismo job para que sus rutas sigan siendo validas.

sonar-project.properties limita el analisis a src/ e include/. Los ejemplos
SFTP, Boost y errores intencionados de Valgrind quedan fuera del alcance inicial.
Los tests se ejecutan, pero no generamos aun un informe de cobertura: pasar los
tests no significa tener cobertura del 100%. Un quality gate que exija cobertura
puede fallar hasta que se incorpore la medicion correspondiente.

`sonar.qualitygate.wait=true` hace fallar el job si no supera las condiciones
de calidad del proyecto. Los detalles se consultan en SonarQube Cloud.
Un workflow rojo no bloquea por si solo un merge: eso se configura mediante
reglas de proteccion de ramas en GitHub. El job Sonar actual, al ejecutarse solo
en la rama principal, no debe configurarse como check obligatorio de las PR.

## Reproducir compilacion y tests localmente

Con CMake, Python 3 y compilador C++ instalados:

```bash
cmake -S . -B build/ci -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/ci --parallel 2
ctest --test-dir build/ci --output-on-failure
```

## Referencias

- https://github.com/SonarSource/sonarqube-scan-action
- https://github.com/sonarsource-cfamily-examples/linux-cmake-compdb-gh-actions-sc
- https://docs.sonarsource.com/sonarqube-cloud/getting-started/github
