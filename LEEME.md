# SolarCleanerPro v2 — Android (APK) + PC (Windows)

Autores: Leon Mena, Ángel Joel · Gallegos Valencia, Cristian Augusto

Tu simulación está en `www/index.html` (sin cambios). Este proyecto la empaqueta como:
- **Android APK** (Capacitor)
- **Windows .exe** instalador y portable (Electron)

## Opción A — Sin instalar nada (recomendada): GitHub Actions
1. Crea un repositorio en github.com y sube todo el contenido de esta carpeta.
2. Ve a **Actions → Build APK y EXE → Run workflow** (o se ejecuta solo al subir).
3. Al terminar (~5-8 min) descarga los **Artifacts**:
   - `SolarCleanerPro-android-apk` → `app-debug.apk` (instalable en cualquier Android)
   - `SolarCleanerPro-windows` → instalador y versión portable `.exe`

## Opción B — En tu computadora
Requisitos: Node 20+, Java 17 y Android Studio (SDK).

**Android:**
```
npm install
npx cap add android
# permitir http local con el ESP32 (una sola vez):
sed -i 's/<application/<application android:usesCleartextTraffic="true"/' android/app/src/main/AndroidManifest.xml
npx @capacitor/assets generate --android
npx cap sync android
cd android && ./gradlew assembleDebug
```
APK: `android/app/build/outputs/apk/debug/app-debug.apk`

**PC (Windows):** `npm install` y luego `npm run dist:win` → carpeta `dist/`.
Probar sin empaquetar: `npm start`.

## APK firmado para Play Store
El APK anterior es de tipo *debug* (instalable directo). Para publicar:
Android Studio → Build → Generate Signed Bundle/APK, con tu propio keystore (guárdalo bien).

## Personalizar
- Nombre/ID: `capacitor.config.json` y `package.json` (`appId`, `productName`).
- Icono: reemplaza `assets/icon.png` (1024×1024) y `build/icon.*`.

## Hoja 3 · Cálculos
La app incluye una hoja de cálculos con: caudal real de agua y presión en las boquillas (curva de la bomba contra curva del sistema), consumo de agua por ciclo y por año, margen eléctrico (cargador, rieles de la fuente ATX) y vida útil por componente en años.
Los valores iniciales son supuestos típicos y se pueden editar en la misma hoja; reemplázalos por los datos de tu bomba, boquilla y motores. Los cambios se guardan en el dispositivo.

## Conexión con el ESP32 (WiFi propio, sin nube)
El programa del ESP32 está en `esp32/` (ver `esp32/LEEME_ESP32.md`). El ESP32 crea la red **SolarCleanerPro**; conecta el teléfono o la PC a esa red y abre la app: **se conecta sola** (dirección `192.168.4.1`), sin tocar ningún botón. Verás en tiempo real el carro, K1, K2, finales de carrera, vueltas y el registro de eventos, y podrás iniciar, detener y controlar a mano. Si no encuentra el ESP32, funciona como simulación y sigue buscándolo; si se pierde la conexión, vuelve a simulación y se reconecta sola.

## Paneles y controles
- La app viene configurada para **3 paneles**. Con el selector **"3 paneles / 4 paneles"** (en "Más controles y estado" o en la Hoja 3) se pasa a 4: el esquema, la longitud de recorrido y los cálculos se actualizan solos.
- En la barra fija están **Bomba** (enciende o apaga K1) y **Escobilla** (enciende la fuente y mueve la escobilla hacia el extremo más lejano; al apagarla se detiene).
- En la Hoja 3 todos los datos son editables (se guardan en el dispositivo).

## Caja de mando
El ESP32 lee un **pulsador de inicio** (GPIO10) y un **hongo de emergencia** (GPIO11). Ver `esp32/LEEME_ESP32.md` y el detalle dibujado en la Hoja 2 de la app. En la Hoja 3 los resultados clave aparecen resaltados en tarjetas.

## Adaptar a la limpieza real
Todos los tiempos del ciclo (fuente, prelavado, pausa, espera final, vueltas, velocidad, bomba solo en prelavado o todo el recorrido) se editan en la Hoja 3 y la simulación, el consumo de agua y las protecciones los usan. La app puede copiar esos valores para pegarlos en el código del ESP32, importar los de tu código o enviarlos al ESP32 conectado (ver `esp32/LEEME_ESP32.md`).

## Diagrama de conexionado en pantalla completa
En la Hoja 2 el diagrama se ajusta al ancho de la pantalla. **+ Zoom** lo agranda, **Ajustar** vuelve al ancho, y **⛶ Pantalla completa** lo abre ocupando todo el teléfono (con pellizco para acercar). Gira el teléfono para verlo más grande.
