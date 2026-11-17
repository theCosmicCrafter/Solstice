from __future__ import annotations

import asyncio
import ctypes
import os
import struct
import sys
import threading
from dataclasses import dataclass
from pathlib import Path

from PyQt6.QtCore import QByteArray
from PyQt6.QtGui import QImage

from .document import SelectionModifiers
from .image import Bounds, Extent, Image, ImageCollection
from .model.jobs import Job, JobKind, JobParams
from .settings import settings
from .util import client_logger, ensure


class NativeEngineError(Exception):
    """The native TRT engine is unavailable or failed at runtime."""


# ---------------------------------------------------------------------------
# Discovery — librediffusion.dll + TRT/CUDA runtime DLLs + engine bundle
# ---------------------------------------------------------------------------


def _appdata_root() -> Path:
    base = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local")
    return Path(base) / "Solstice"


def _install_root() -> Path:
    # <install>/bin/krita.dll -> <install>
    bin_dir = Path(sys.executable).resolve().parent
    return bin_dir.parent if bin_dir.name.lower() == "bin" else bin_dir


def dll_directories() -> list[Path]:
    """Directories that must be on the DLL search path before loading."""
    dirs: list[Path] = []
    candidates = [
        os.environ.get("SOLSTICE_LIBREDIFFUSION_DIR"),
        str(_install_root() / "share" / "krita" / "livediffusion" / "bin"),
        str(_appdata_root() / "livediffusion" / "bin"),
    ]
    trt = os.environ.get("TENSORRT_DIR")
    if trt:
        candidates += [str(Path(trt) / "bin"), str(Path(trt) / "lib")]
    cuda = os.environ.get("CUDA_PATH")
    if cuda:
        candidates += [str(Path(cuda) / "bin" / "x64"), str(Path(cuda) / "bin")]
    for c in candidates:
        if c and Path(c).is_dir() and Path(c) not in dirs:
            dirs.append(Path(c))
    return dirs


def find_library() -> Path | None:
    candidates = [
        os.environ.get("SOLSTICE_LIBREDIFFUSION_DIR"),
        str(_install_root() / "share" / "krita" / "livediffusion" / "bin"),
        str(_appdata_root() / "livediffusion" / "bin"),
    ]
    for root in candidates:
        if root and (Path(root) / "librediffusion.dll").is_file():
            return Path(root) / "librediffusion.dll"
    return None


@dataclass
class EngineBundle:
    """File set a klein stream needs (built by librediffusion's train-lora.py)."""

    root: Path
    transformer: Path
    qwen_encoder: Path
    vae_decoder: Path
    vae_encoder: Path
    tokenizer: Path
    bn_mean: Path
    bn_std: Path
    rife: Path | None = None

    def check(self) -> str | None:
        for name in (
            "transformer",
            "qwen_encoder",
            "vae_decoder",
            "vae_encoder",
            "tokenizer",
            "bn_mean",
            "bn_std",
        ):
            if not getattr(self, name).is_file():
                return f"missing {name}: {getattr(self, name)}"
        return None


def find_engine_bundle(kind: str = "klein") -> EngineBundle | None:
    names = {
        "klein": dict(
            transformer="transformer_bf16.plan",
            qwen_encoder="qwen3_encoder_bf16.plan",
            vae_decoder="vae_decoder_bf16.plan",
            vae_encoder="vae_encoder_bf16.plan",
            tokenizer="tokenizer.json",
            bn_mean="bn_mean.bin",
            bn_std="bn_std.bin",
            rife="rife_ifnet_fp16.plan",
        )
    }[kind]
    roots = [
        settings.native_engine_dir,
        os.environ.get("SOLSTICE_LIVEDIFFUSION_ENGINES", ""),
        str(_appdata_root() / "livediffusion" / "engines" / kind),
        str(_install_root() / "share" / "krita" / "livediffusion" / "engines" / kind),
    ]
    for root in roots:
        if not root:
            continue
        n = dict(names)
        if settings.native_precision == "fp8":
            fp8 = Path(root) / "transformer_fp8.plan"
            if fp8.is_file():
                n["transformer"] = fp8.name
        bundle = EngineBundle(root=Path(root), **{k: Path(root) / v for k, v in n.items()})
        if bundle.transformer.is_file():
            return bundle
    return None


# ---------------------------------------------------------------------------
# ctypes bindings (subset — stream API only; see librediffusion_c.h)
# ---------------------------------------------------------------------------


class _Lib:
    _instance: ctypes.CDLL | None = None
    _load_error: str | None = None

    @classmethod
    def get(cls) -> ctypes.CDLL:
        if cls._instance is not None:
            return cls._instance
        if cls._load_error is not None:
            raise NativeEngineError(cls._load_error)
        dll = find_library()
        if dll is None:
            cls._load_error = (
                "librediffusion.dll not found (searched SOLSTICE_LIBREDIFFUSION_DIR, "
                "install share/krita/livediffusion/bin, and the app-data livediffusion dir)"
            )
            raise NativeEngineError(cls._load_error)
        try:
            os.add_dll_directory(str(dll.parent))
            for d in dll_directories():
                os.add_dll_directory(str(d))
            cls._instance = ctypes.CDLL(str(dll))
        except OSError as e:
            cls._load_error = f"failed to load {dll}: {e}"
            raise NativeEngineError(cls._load_error)

        lib = cls._instance
        h = ctypes.c_void_p
        lib.librediffusion_version.restype = ctypes.c_char_p
        lib.librediffusion_error_string.argtypes = [ctypes.c_int]
        lib.librediffusion_error_string.restype = ctypes.c_char_p
        lib.librediffusion_flux2_stream_create.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_ulonglong,
            ctypes.c_int,
        ]
        lib.librediffusion_flux2_stream_create.restype = h
        lib.librediffusion_flux2_stream_destroy.argtypes = [h]
        lib.librediffusion_flux2_stream_dims.argtypes = [
            h,
            ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int),
        ]
        lib.librediffusion_flux2_stream_set_prompt.argtypes = [h, ctypes.c_char_p]
        lib.librediffusion_flux2_stream_set_prompt.restype = ctypes.c_int
        lib.librediffusion_flux2_stream_set_strength.argtypes = [h, ctypes.c_float]
        lib.librediffusion_flux2_stream_set_schedule.argtypes = [
            h,
            ctypes.POINTER(ctypes.c_float),
            ctypes.c_int,
        ]
        lib.librediffusion_flux2_stream_set_bn.argtypes = [
            h,
            ctypes.POINTER(ctypes.c_float),
            ctypes.POINTER(ctypes.c_float),
        ]
        lib.librediffusion_flux2_stream_set_bn.restype = ctypes.c_int
        lib.librediffusion_flux2_stream_set_mask.argtypes = [h, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
        lib.librediffusion_flux2_stream_set_reference.argtypes = [h, ctypes.c_char_p]
        lib.librediffusion_flux2_stream_set_reference.restype = ctypes.c_int
        lib.librediffusion_flux2_stream_frame_cached.argtypes = [h, ctypes.c_char_p]
        lib.librediffusion_flux2_stream_frame_cached.restype = ctypes.c_int
        lib.librediffusion_flux2_stream_frame.argtypes = [h, ctypes.c_char_p, ctypes.c_char_p]
        lib.librediffusion_flux2_stream_frame.restype = ctypes.c_int
        lib.librediffusion_rife_create.argtypes = [ctypes.c_char_p, ctypes.c_int]
        lib.librediffusion_rife_create.restype = h
        lib.librediffusion_rife_destroy.argtypes = [h]
        lib.librediffusion_rife_set_enabled.argtypes = [h, ctypes.c_int]
        lib.librediffusion_rife_set_interpolation_exp.argtypes = [h, ctypes.c_int]
        lib.librediffusion_rife_interpolate_sized.argtypes = [
            h,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_int),
        ]
        lib.librediffusion_rife_interpolate_sized.restype = ctypes.c_int
        return lib

    @classmethod
    def version(cls) -> str:
        return cls.get().librediffusion_version().decode("utf-8")

    @classmethod
    def error_string(cls, code: int) -> str:
        try:
            return cls.get().librediffusion_error_string(code).decode("utf-8")
        except Exception:
            return f"error {code}"


def _check(code: int, what: str):
    if code != 0:
        raise NativeEngineError(f"{what}: {_Lib.error_string(code)} ({code})")


def _f32_array(path: Path, count: int) -> ctypes.Array[ctypes.c_float]:
    data = path.read_bytes()
    expected = count * 4
    if len(data) != expected:
        raise NativeEngineError(f"{path.name}: expected {expected} bytes, got {len(data)}")
    return (ctypes.c_float * count)(*struct.unpack(f"<{count}f", data))


def _image_to_rgba_bytes(image: Image) -> tuple[bytes, int, int]:
    qimg = image._qimage
    if qimg.format() != QImage.Format.Format_RGBA8888:
        qimg = qimg.convertToFormat(QImage.Format.Format_RGBA8888)
    ptr = qimg.constBits()
    return bytes(ptr.asstring(qimg.sizeInBytes())), qimg.height(), qimg.width()


def _rgba_bytes_to_image(data: bytes | bytearray, width: int, height: int) -> Image:
    qimg = QImage(bytes(data), width, height, width * 4, QImage.Format.Format_RGBA8888)
    return Image(qimg.copy())


# ---------------------------------------------------------------------------
# Klein stream wrapper
# ---------------------------------------------------------------------------


class KleinStream:
    def __init__(self, bundle: EngineBundle, token_h: int, token_w: int, seed: int, device: int):
        self._bundle = bundle
        self._lib = _Lib.get()
        # Set before any fallible call so close()/__del__ are always safe.
        self._handle = None
        self._rife = None
        self._rife_exp = 0
        self._last_bytes: bytes | None = None
        # Serializes C calls against close(): the GUI thread must not destroy a
        # handle while a worker thread is inside a DLL call.
        self._lock = threading.Lock()
        handle = self._lib.librediffusion_flux2_stream_create(
            str(bundle.transformer).encode("utf-8"),
            str(bundle.qwen_encoder).encode("utf-8"),
            str(bundle.vae_decoder).encode("utf-8"),
            str(bundle.vae_encoder).encode("utf-8"),
            str(bundle.tokenizer).encode("utf-8"),
            token_h,
            token_w,
            seed,
            device,
        )
        if not handle:
            raise NativeEngineError("flux2_stream_create returned NULL (engine load failed)")
        self._handle = handle
        self.height, self.width = token_h * 16, token_w * 16
        self._out = ctypes.create_string_buffer(self.height * self.width * 4)
        mean = _f32_array(bundle.bn_mean, 128)
        std = _f32_array(bundle.bn_std, 128)
        _check(
            self._lib.librediffusion_flux2_stream_set_bn(self._handle, mean, std),
            "flux2_stream_set_bn",
        )

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def close(self):
        with self._lock:
            if self._rife is not None:
                self._lib.librediffusion_rife_destroy(self._rife)
                self._rife = None
            if self._handle:
                self._lib.librediffusion_flux2_stream_destroy(self._handle)
                self._handle = None

    def enable_rife(self, plan_path: Path, exp: int):
        """Attach the RIFE interpolator (opt-in). exp 1-3 -> 2x/4x/8x frames."""
        exp = max(0, min(int(exp), 3))  # cap output buffer at 8x frame size
        if self._rife is not None or exp <= 0 or not plan_path.is_file():
            return
        h = self._lib.librediffusion_rife_create(str(plan_path).encode("utf-8"), 0)
        if not h:
            client_logger.warning("Native engine: RIFE create failed; continuing without it")
            return
        self._lib.librediffusion_rife_set_enabled(h, 1)
        self._lib.librediffusion_rife_set_interpolation_exp(h, exp)
        self._rife = h
        self._rife_exp = exp

    def rife_frames(self, prev: bytes | None, cur: bytes) -> list[bytes]:
        """Interpolated frames prev->cur at stream dims; last element is the real cur."""
        if self._rife is None or prev is None:
            return [cur]
        fsize = self.height * self.width * 4
        cap = (1 << self._rife_exp) * fsize
        out = ctypes.create_string_buffer(cap)
        n = ctypes.c_int(0)
        _check(
            self._lib.librediffusion_rife_interpolate_sized(
                self._rife, prev, cur, self.height, self.width, out, cap, ctypes.byref(n)
            ),
            "rife_interpolate",
        )
        count = min(max(n.value, 0), cap // fsize)
        return [bytes(out.raw[i * fsize : (i + 1) * fsize]) for i in range(count)] or [cur]

    def set_prompt(self, prompt: str):
        tokens = self._lib.librediffusion_flux2_stream_set_prompt(
            self._handle, prompt.encode("utf-8")
        )
        if tokens < 0:
            raise NativeEngineError("flux2_stream_set_prompt failed")

    def set_schedule(self, sigmas: list[float]):
        arr = (ctypes.c_float * len(sigmas))(*sigmas)
        self._lib.librediffusion_flux2_stream_set_schedule(self._handle, arr, len(sigmas))

    def set_mask(self, mask_rgba: bytes, height: int, width: int):
        self._lib.librediffusion_flux2_stream_set_mask(
            self._handle, mask_rgba, height, width
        )

    def set_reference(self, rgba: bytes):
        _check(
            self._lib.librediffusion_flux2_stream_set_reference(self._handle, rgba),
            "flux2_stream_set_reference",
        )

    def frame_cached(self) -> Image:
        _check(
            self._lib.librediffusion_flux2_stream_frame_cached(self._handle, self._out),
            "flux2_stream_frame_cached",
        )
        self._last_bytes = bytes(self._out.raw)
        return _rgba_bytes_to_image(self._out.raw, self.width, self.height)


# ---------------------------------------------------------------------------
# NativeLiveEngine — owned by LiveWorkspace, one stream per document
# ---------------------------------------------------------------------------

DEFAULT_SEED = 52
DEFAULT_SCHEDULE = [1.0, 0.64]


class NativeLiveEngine:
    """Drives a klein stream for one document's Live mode.

    Produces finished `JobKind.live_preview` jobs through the normal JobQueue,
    so result preview/apply reuse the existing LiveWorkspace machinery.
    """

    def __init__(self, model):
        self._model = model
        self._stream: KleinStream | None = None
        self._stream_dims: tuple[int, int] | None = None  # (H, W) in pixels
        self._unavailable_reason: str | None = None
        self._busy = False
        self._last_fingerprint = None
        self._last_capture_error: str | None = None
        self._prev_frame: bytes | None = None
        self._frame_index = 0

    # -- selection -----------------------------------------------------------

    def is_selected(self) -> bool:
        return settings.live_engine == "native"

    def availability(self) -> str | None:
        """None if usable, else a human-readable reason (also used by UI)."""
        if self._unavailable_reason:
            return self._unavailable_reason
        try:
            _Lib.get()
            bundle = find_engine_bundle()
            missing = bundle.check() if bundle else None
        except Exception as e:
            return f"engine check failed: {e}"
        if bundle is None:
            return "no klein engine bundle found (train engines or set the engine directory)"
        if missing:
            return f"engine bundle at {bundle.root}: {missing}"
        return None

    # -- stream lifecycle ----------------------------------------------------

    # Engine geometry: the klein bundle covers canvas-fitted token grids —
    # Th in [18,48], Tw in [16,48] (VAE profile min/opt/max), with a token
    # budget of 2304 image tokens (transformer Lp max = 4608 incl. ref).
    # The frame is scaled to the chosen stream dims, the result scaled back.
    _MIN_TH, _MIN_TW = 18, 16
    _MAX_TH, _MAX_TW = 48, 48
    _TOKEN_BUDGET = 2304

    def _token_grid(self, extent) -> tuple[int, int]:
        th = max(self._MIN_TH, min(self._MAX_TH, extent.height // 16))
        tw = max(self._MIN_TW, min(self._MAX_TW, extent.width // 16))
        while th * tw > self._TOKEN_BUDGET:
            if th >= tw and th > self._MIN_TH:
                th -= 1
            else:
                tw -= 1
        return th, tw

    def _ensure_stream(self, extent) -> KleinStream:
        th, tw = self._token_grid(extent)
        dims = (th * 16, tw * 16)
        if self._stream is not None and self._stream_dims == dims:
            return self._stream
        self._destroy_stream()
        reason = self.availability()
        if reason:
            raise NativeEngineError(reason)
        bundle = ensure(find_engine_bundle())
        client_logger.info(
            f"Native engine: creating klein stream {dims[1]}x{dims[0]} from {bundle.root}"
        )
        self._stream = KleinStream(bundle, th, tw, DEFAULT_SEED, device=0)
        if bundle.rife and bundle.rife.is_file():
            self._stream.enable_rife(bundle.rife, settings.native_rife_exp)
        self._stream_dims = dims
        self._last_fingerprint = None
        self._prev_frame = None
        return self._stream

    def _destroy_stream(self):
        if self._stream is not None:
            self._stream.close()
            self._stream = None
            self._stream_dims = None
            self._prev_frame = None

    def release(self):
        self._destroy_stream()
        self._unavailable_reason = None

    # -- frame generation ----------------------------------------------------

    def _capture(self) -> tuple[JobParams, str, float, bytes | None, int, int, Image]:
        """Collect inputs on the GUI thread (Krita APIs are GUI-only)."""
        model = self._model
        extent = model._doc.extent
        bounds = Bounds(0, 0, *extent)
        image = model._get_current_image(bounds, exclude_internal=False)

        prompt = model.active_regions.active_or_root.positive.strip()
        strength = model.live.strength

        mask_img = None
        mask, _sel = model._doc.create_mask_from_selection(
            SelectionModifiers(pad_rel=0.02, multiple=16)
        )
        if mask is not None:
            mask_img = mask.to_image(bounds.extent)

        params = JobParams(bounds, prompt or "[Live]")
        return params, prompt, strength, mask_img, image

    def _run_frame(self, prompt, strength, mask_bytes, ref_bytes, stream) -> Image:
        with stream._lock:
            # set_prompt is required even when empty: frame_cached rejects calls
            # until a prompt has been encoded (same prompt is a cached no-op).
            stream.set_prompt(prompt or "")
            schedule = (
                [strength, round(strength * 0.64, 4)] if strength < 1.0 else DEFAULT_SCHEDULE
            )
            stream.set_schedule(schedule)
            if mask_bytes is not None:
                stream.set_mask(mask_bytes, stream.height, stream.width)
            stream.set_reference(ref_bytes)
            return stream.frame_cached()

    async def generate_frame(self) -> bool:
        """One live frame on the native engine. Returns False when skipped."""
        if self._busy:
            return False
        model = self._model
        try:
            params, prompt, strength, mask_img, image = self._capture()
            self._last_capture_error = None
        except Exception as e:
            if str(e) != self._last_capture_error:
                self._last_capture_error = str(e)
                model.report_error(str(e))
            return False

        fingerprint = (prompt, strength, mask_img is not None, image._qimage.cacheKey())
        if fingerprint == self._last_fingerprint:
            return False

        job = self._model.jobs.add_job(Job(f"native-{self._frame_index}", JobKind.live_preview, params))
        self._frame_index += 1
        self._busy = True
        try:
            stream = self._ensure_stream(params.bounds.extent)
            s_extent = Extent(stream.width, stream.height)
            ref_img = image if image.extent == s_extent else Image.scale(image, s_extent)
            ref_bytes, _h, _w = _image_to_rgba_bytes(ref_img)
            mask_bytes = None
            if mask_img is not None:
                mask_img = (
                    mask_img if mask_img.extent == s_extent else Image.scale(mask_img, s_extent)
                )
                mask_bytes, _h, _w = _image_to_rgba_bytes(mask_img)
            result = await asyncio.to_thread(
                self._run_frame, prompt, strength, mask_bytes, ref_bytes, stream
            )
            # RIFE intermediates: display-only previews between the previous and
            # current real frames. The real frame still arrives via the Job path.
            cur_bytes = stream._last_bytes
            if cur_bytes is not None:
                frames = stream.rife_frames(self._prev_frame, cur_bytes)
                self._prev_frame = cur_bytes
                for fb in frames[:-1]:
                    img = _rgba_bytes_to_image(fb, stream.width, stream.height)
                    if img.extent != params.bounds.extent:
                        img = Image.scale(img, params.bounds.extent)
                    model.live.set_result(img, params)
            if result.extent != params.bounds.extent:
                result = Image.scale(result, params.bounds.extent)
            self._model.jobs.set_results(job, ImageCollection([result]))
            self._model.jobs.notify_finished(job)
            self._last_fingerprint = fingerprint
            return True
        except Exception as e:
            # Any native failure (engine error, filesystem, conversion) degrades
            # to the ComfyUI path rather than stalling the live loop.
            self._unavailable_reason = str(e)
            self._destroy_stream()
            model.report_error(f"Native engine failed, falling back to ComfyUI: {e}")
            self._model.jobs.notify_cancelled(job)
            return False
        finally:
            self._busy = False
