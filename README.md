# AcoustID AutoTag - Plugin Winamp (estilo Shazam)

Detecta la cancion por fingerprint y escribe los TAGS ID3v2 (Artista,
Titulo, Album) en el MP3. NO renombra el archivo.

## Compilar
Abre AcoustIDPlugin.sln en Visual Studio > Release + x86 > Ctrl+Shift+B
DLL: AcoustIDPlugin\Release\gen_acoustid.dll

## Instalar
- gen_acoustid.dll -> <Winamp>\Plugins\
- fpcalc.exe       -> <Winamp>\Plugins\   (https://acoustid.org/chromaprint)
- Reiniciar Winamp

## API key
Application key de https://acoustid.org/new-application (NO la user key)

## Configurar
Preferencias > Plug-ins > General Purpose > AcoustID AutoTag > Configure

## Escritura sin parar el audio (nuevo)
El plugin intenta escribir el tag de DOS formas:

1. IN-PLACE (no para el audio):
   Si el MP3 ya tiene un tag ID3v2 con espacio suficiente, sobreescribe
   los frames sin mover el audio. Abre el archivo en modo compartido,
   asi que Winamp puede seguir reproduciendo. Funciona al instante.

2. RECREAR (si no cabe in-place):
   Si el archivo no tiene tag o no hay espacio, se recrea el MP3 con un
   tag nuevo + 1KB de padding. Esto requiere que el archivo NO este en uso.
   Si esta sonando, el cambio queda PENDIENTE y se aplica automaticamente
   en cuanto cambias de pista (el plugin lo detecta y escribe solo).

Resultado: la primera vez que taggeas un archivo nuevo puede quedar
pendiente hasta cambiar de pista; pero como deja 1KB de padding, las
siguientes ediciones de ese archivo son in-place instantaneas sin parar nada.

## Notas
- Tags en ID3v2.3 / ISO-8859-1. Para acentos perfectos faltaria UTF-16 (futuro).
- Compilaciones muy oscuras pueden no estar en la base de AcoustID (score 0%).

## Actualizacion de la Media Library (columnas Artista/Titulo/Album)

El plugin escribe los tags ID3 en campos SEPARADOS:
  TPE1 = Artista   TIT2 = Titulo   TALB = Album

Para que las COLUMNAS de la Media Library se actualicen:

1. El plugin ahora actualiza la FECHA DE MODIFICACION del archivo
   (SetFileTime) tras escribir el tag. Esto es necesario porque Winamp
   solo re-escanea archivos cuya fecha cambio.

2. La ML se refresca:
   - Automaticamente en el proximo escaneo de carpetas vigiladas
   - O al instante: en la Media Library, selecciona los archivos,
     clic derecho > "Read Metadata on Selected Items" (Leer metadatos),
     luego "Clear Search" para refrescar las listas de Artista/Album.

Nota: Winamp NO tiene un IPC publico para forzar el rescan completo de la
ML desde un plugin. El truco de la fecha de modificacion garantiza que el
rescan automatico SI tome los cambios (antes los ignoraba en escrituras
in-place que no cambiaban el tamanio del archivo).

## Soporte Unicode / Japones / Acentos (v1.1)

Los tags ahora se escriben en ID3v2.3 con encoding UTF-16 (con BOM).
El parser JSON decodifica las secuencias \uXXXX que devuelve AcoustID,
asi que titulos en japones, cirilico, acentos, etc. se guardan y muestran
correctamente (antes salian como "\u30a2\u30f3..." literal).
