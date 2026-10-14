# Rest Note

Rest Note is a native timer docker with working, paused, idle, eye-break, and
full-break states. It keeps the icons and settings of the Rest Note plugin it replaces.

For details, see the original plugin repository, [Rest
Note](https://github.com/udtren/krita-rest-note). This guide covers
Solstice's native version and where it differs.

The small eye-break notification appears on the screen Solstice is on. The large
break overlay is limited to the Solstice window rather than covering the entire
monitor.

Rest Note is off by default. Turn it on in **Settings > Configure Solstice > General >
Custom > Enable the Rest Note docker**, restart Solstice, and show it from
**Settings > Dockers**. Turning the option off stops the timers and any break
notice at once and hides the docker; it disappears from the Dockers menu
after the next restart.

Native implementation: [`../plugins/dockers/restnote`](../plugins/dockers/restnote/)
