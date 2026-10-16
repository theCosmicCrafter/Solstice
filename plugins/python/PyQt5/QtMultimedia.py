# SPDX-License-Identifier: GPL-3.0-or-later
#
# PyQt5.QtMultimedia -> PyQt6.QtMultimedia
#
# Qt6 removed QMediaContent and changed QMediaPlayer's API
# (setMedia -> setSource, volume moved to QAudioOutput). Wrapper classes
# below keep the Qt5 surface used by Krita plugins working.

from PyQt6.QtMultimedia import *  # noqa: F401,F403
from PyQt6.QtMultimedia import QMediaPlayer as _QMediaPlayer
from PyQt6.QtMultimedia import QAudioOutput
from PyQt6.QtCore import QUrl


class QMediaContent:
    """Qt5 QMediaContent stand-in: a thin wrapper around a single QUrl."""

    def __init__(self, content=None):
        if isinstance(content, QMediaContent):
            self._url = QUrl(content._url)
        elif isinstance(content, QUrl):
            self._url = QUrl(content)
        elif content is None:
            self._url = QUrl()
        else:
            self._url = QUrl(str(content))

    def isNull(self):
        return self._url.isEmpty() or not self._url.isValid()

    def canonicalUrl(self):
        return QUrl(self._url)

    def canonicalResource(self):
        return self


class QMediaPlayer(_QMediaPlayer):
    """Qt5-style media player on top of the Qt6 class.

    - setMedia(QMediaContent/QUrl) -> setSource(url)
    - setVolume(int 0-100) -> QAudioOutput.setVolume(float 0-1)
    """

    def __init__(self, parent=None, *args):
        # Qt5 took extra flag arguments Qt6 dropped; accept and ignore them.
        super().__init__(parent)
        self._compat_audio = QAudioOutput(self)
        self.setAudioOutput(self._compat_audio)

    def setMedia(self, content, stream=None):  # noqa: N802 - Qt5 API name
        if isinstance(content, QMediaContent):
            url = content.canonicalUrl()
        elif isinstance(content, QUrl):
            url = content
        elif content:
            url = QUrl(str(content))
        else:
            url = QUrl()
        self.setSource(url)

    def media(self):  # noqa: N802 - Qt5 API name
        return QMediaContent(self.source())

    def setVolume(self, volume):  # noqa: N802 - Qt5 API name
        # Qt5 used an int percentage; Qt6 uses a float 0..1 on QAudioOutput.
        v = float(volume)
        if v > 1.0:
            v = v / 100.0
        self._compat_audio.setVolume(v)

    def volume(self):  # noqa: N802 - Qt5 API name
        return int(round(self._compat_audio.volume() * 100.0))

    def setMuted(self, muted):  # noqa: N802 - Qt5 API name
        self._compat_audio.setMuted(bool(muted))

    def isMuted(self):  # noqa: N802 - Qt5 API name
        return self._compat_audio.isMuted()


try:
    from PyQt6.QtMultimedia import QSoundEffect

    class QSound:
        """Minimal Qt5 QSound stand-in using QSoundEffect."""
        @staticmethod
        def play(filename):
            effect = QSoundEffect()
            effect.setSource(QUrl.fromLocalFile(filename))
            effect.play()
except ImportError:
    pass
