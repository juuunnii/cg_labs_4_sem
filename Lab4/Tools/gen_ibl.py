"""
Предварительный расчёт карт для IBL (Image Based Lighting).

Генерирует три файла .dds, которые загружает программа:
  irradiance.dds   — irradiance map (кубическая, 32x32): свёртка неба с косинусом,
                     даёт диффузное освещение со всех направлений
  prefiltered.dds  — pre-filtered environment map (кубическая, 128x128, 5 mip-уровней):
                     небо, размытое по GGX; mip 0 — зеркальное отражение (roughness 0),
                     mip 4 — полностью матовое (roughness 1)
  brdf_lut.dds     — BRDF integration map (128x128, R16G16): вторая часть split-sum
                     аппроксимации Карриса, зависит от (N·V, roughness)

Источник освещения — процедурное HDR-небо (градиент + солнце), направление солнца
совпадает с направленным источником в программе.

Запуск:  python gen_ibl.py [папка_вывода]
Нужен только numpy.
"""
import sys
import os
import struct
import numpy as np

OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else "."

# Направление, КУДА светит солнце в программе (LightData.Direction) -> солнце на небе в обратной стороне
SUN_LIGHT_DIR = np.array([0.3, -1.0, 0.25])
SUN_DIR = -SUN_LIGHT_DIR / np.linalg.norm(SUN_LIGHT_DIR)

ENV_SIZE = 128            # размер грани pre-filtered map (mip 0)
PREFILTER_MIPS = 5        # roughness = mip / (MIPS - 1)
IRRADIANCE_SIZE = 32
LUT_SIZE = 128
LUT_SAMPLES = 1024


# ----------------------------------------------------------------------------------
# Процедурное HDR-небо
# ----------------------------------------------------------------------------------
def sky(d):
    """d: (..., 3) нормализованные направления -> (..., 3) HDR-радиантность"""
    y = d[..., 1:2]

    zenith  = np.array([0.18, 0.36, 0.85]) * 1.4
    horizon = np.array([0.85, 0.88, 0.95]) * 1.2
    ground  = np.array([0.30, 0.26, 0.22]) * 0.5

    t_up = np.sqrt(np.clip(y, 0.0, 1.0))
    upper = horizon * (1.0 - t_up) + zenith * t_up
    t_dn = np.clip(-y * 4.0, 0.0, 1.0)
    lower = (horizon * 0.6) * (1.0 - t_dn) + ground * t_dn
    col = np.where(y >= 0.0, upper, lower)

    # Солнце: яркий диск + ореол
    cos_sun = np.sum(d * SUN_DIR, axis=-1, keepdims=True)
    disk = (cos_sun > np.cos(np.radians(2.5))).astype(np.float64) * 25.0
    glow = np.exp((cos_sun - 1.0) * 60.0) * 1.5
    col = col + (disk + glow) * np.array([1.0, 0.92, 0.78])
    return col


# ----------------------------------------------------------------------------------
# Кубические карты: направления текселей и их телесные углы
# ----------------------------------------------------------------------------------
def face_dirs(face, size):
    """Направления центров текселей грани (порядок граней D3D: +X,-X,+Y,-Y,+Z,-Z)."""
    c = (np.arange(size) + 0.5) / size * 2.0 - 1.0
    u, v = np.meshgrid(c, c)       # u — вправо, v — вниз
    one = np.ones_like(u)
    if   face == 0: d = np.stack([ one, -v, -u], -1)
    elif face == 1: d = np.stack([-one, -v,  u], -1)
    elif face == 2: d = np.stack([ u,  one,  v], -1)
    elif face == 3: d = np.stack([ u, -one, -v], -1)
    elif face == 4: d = np.stack([ u, -v,  one], -1)
    else:           d = np.stack([-u, -v, -one], -1)
    solid = (2.0 / size) ** 2 / (1.0 + u * u + v * v) ** 1.5
    return d / np.linalg.norm(d, axis=-1, keepdims=True), solid


def cube_dirs(size):
    dirs, solids = zip(*(face_dirs(f, size) for f in range(6)))
    return np.stack(dirs), np.stack(solids)          # (6,S,S,3), (6,S,S)


def render_env(size, supersample=4):
    """Небо в кубическую карту с усреднением (сохраняет энергию солнца при малом разрешении)."""
    hi, _ = cube_dirs(size * supersample)
    rad = sky(hi)
    return rad.reshape(6, size, supersample, size, supersample, 3).mean(axis=(2, 4))


# ----------------------------------------------------------------------------------
# Свёртки
# ----------------------------------------------------------------------------------
def ggx_d(n_dot_h, alpha):
    a2 = alpha * alpha
    denom = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0
    return a2 / (np.pi * denom * denom)


def convolve(out_size, src_size, weight_fn):
    """Для каждого выходного направления R: сумма L(l)*w(R,l)*dΩ / сумма w(R,l)*dΩ."""
    src = render_env(src_size)
    src_dirs, src_solid = cube_dirs(src_size)
    L = src.reshape(-1, 3).astype(np.float32)
    ldir = src_dirs.reshape(-1, 3).astype(np.float32)
    lsolid = src_solid.reshape(-1).astype(np.float32)

    out_dirs, _ = cube_dirs(out_size)
    R = out_dirs.reshape(-1, 3).astype(np.float32)
    result = np.zeros((R.shape[0], 3), np.float32)

    chunk = 512
    for i in range(0, R.shape[0], chunk):
        r = R[i:i + chunk]
        w = weight_fn(r, ldir) * lsolid[None, :]
        result[i:i + chunk] = (w @ L) / np.maximum(w.sum(axis=1, keepdims=True), 1e-8)
    return result.reshape(6, out_size, out_size, 3)


def irradiance_weight(r, ldir):
    # Косинусная свёртка по полусфере вокруг нормали r
    return np.maximum(r @ ldir.T, 0.0)


def make_prefilter_weight(roughness):
    alpha = max(roughness * roughness, 1e-4)

    def w(r, ldir):
        # Приближение Карриса: N = V = R. Вес = D(n·h) * (n·l)
        n_dot_l = r @ ldir.T
        h = r[:, None, :] + ldir[None, :, :]
        h /= np.maximum(np.linalg.norm(h, axis=-1, keepdims=True), 1e-8)
        n_dot_h = np.clip(np.sum(h * r[:, None, :], axis=-1), 0.0, 1.0)
        return np.where(n_dot_l > 0.0, ggx_d(n_dot_h, alpha) * n_dot_l, 0.0).astype(np.float32)
    return w


# ----------------------------------------------------------------------------------
# BRDF LUT (split-sum, вторая часть)
# ----------------------------------------------------------------------------------
def hammersley(n):
    i = np.arange(n, dtype=np.uint32)
    bits = i.copy()
    bits = ((bits << 16) | (bits >> 16)) & 0xFFFFFFFF
    bits = ((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1)
    bits = ((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2)
    bits = ((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4)
    bits = ((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8)
    return i / n, bits.astype(np.float64) / 4294967296.0


def brdf_lut(size, samples):
    n_dot_v = (np.arange(size) + 0.5) / size           # x
    rough = (np.arange(size) + 0.5) / size             # y
    NV, RO = np.meshgrid(n_dot_v, rough)               # (size, size)

    V = np.stack([np.sqrt(1.0 - NV * NV), np.zeros_like(NV), NV], -1)   # N = (0,0,1)
    alpha = RO * RO
    xi1, xi2 = hammersley(samples)

    A = np.zeros_like(NV)
    B = np.zeros_like(NV)
    k = alpha / 2.0                                     # k для IBL
    for s in range(samples):
        # Выборка полувектора H по GGX
        phi = 2.0 * np.pi * xi1[s]
        cos_t = np.sqrt((1.0 - xi2[s]) / (1.0 + (alpha * alpha - 1.0) * xi2[s]))
        sin_t = np.sqrt(1.0 - cos_t * cos_t)
        H = np.stack([sin_t * np.cos(phi), sin_t * np.sin(phi), cos_t], -1)
        v_dot_h = np.sum(V * H, -1)
        Lz = 2.0 * v_dot_h * H[..., 2] - V[..., 2]      # L = reflect(-V, H), нужна только z = N·L
        n_dot_l = np.clip(Lz, 0.0, 1.0)
        n_dot_h = np.clip(H[..., 2], 0.0, 1.0)
        v_dot_h = np.clip(v_dot_h, 0.0, 1.0)

        g_v = NV / (NV * (1.0 - k) + k)
        g_l = n_dot_l / (n_dot_l * (1.0 - k) + k)
        G = g_v * g_l
        G_vis = np.where(n_dot_l > 0.0, G * v_dot_h / np.maximum(n_dot_h * NV, 1e-8), 0.0)
        Fc = (1.0 - v_dot_h) ** 5
        A += (1.0 - Fc) * G_vis
        B += Fc * G_vis
    return np.stack([A / samples, B / samples], -1)     # (size, size, 2)


# ----------------------------------------------------------------------------------
# Запись DDS (заголовок DX10)
# ----------------------------------------------------------------------------------
DXGI_R16G16B16A16_FLOAT = 10
DXGI_R16G16_FLOAT = 34


def write_dds(path, width, height, mips, dxgi_format, is_cube, data_bytes):
    DDSD = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000
    caps = 0x1000 | (0x8 | 0x400000 if (mips > 1 or is_cube) else 0)
    caps2 = 0xFE00 if is_cube else 0

    header = struct.pack("<4s", b"DDS ")
    header += struct.pack("<7I", 124, DDSD, height, width, 0, 0, mips)
    header += struct.pack("<11I", *([0] * 11))
    header += struct.pack("<2I4s5I", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)   # pixel format -> DX10
    header += struct.pack("<5I", caps, caps2, 0, 0, 0)
    header += struct.pack("<5I", dxgi_format, 3, 0x4 if is_cube else 0, 1, 0)
    with open(path, "wb") as f:
        f.write(header)
        f.write(data_bytes)


def rgba16(rgb):
    a = np.ones(rgb.shape[:-1] + (1,), rgb.dtype)
    return np.concatenate([rgb, a], -1).astype(np.float16)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    # ---- Irradiance map ----
    print("irradiance map ...")
    irr = convolve(IRRADIANCE_SIZE, 32, irradiance_weight)
    write_dds(os.path.join(OUT_DIR, "irradiance.dds"), IRRADIANCE_SIZE, IRRADIANCE_SIZE, 1,
              DXGI_R16G16B16A16_FLOAT, True, rgba16(irr).tobytes())

    # ---- Pre-filtered environment map ----
    levels = []
    for mip in range(PREFILTER_MIPS):
        size = ENV_SIZE >> mip
        rough = mip / (PREFILTER_MIPS - 1)
        print(f"prefiltered mip {mip}: {size}x{size}, roughness {rough:.2f} ...")
        if mip == 0:
            levels.append(render_env(size))                  # roughness 0 = чистое отражение
        else:
            src = 64 if mip == 1 else 32
            levels.append(convolve(size, src, make_prefilter_weight(rough)))
    # Порядок в DDS: для каждой грани — все её mip-уровни
    blob = b"".join(rgba16(levels[m][face]).tobytes()
                    for face in range(6) for m in range(PREFILTER_MIPS))
    write_dds(os.path.join(OUT_DIR, "prefiltered.dds"), ENV_SIZE, ENV_SIZE, PREFILTER_MIPS,
              DXGI_R16G16B16A16_FLOAT, True, blob)

    # ---- BRDF integration map ----
    print("BRDF LUT ...")
    lut = brdf_lut(LUT_SIZE, LUT_SAMPLES)
    write_dds(os.path.join(OUT_DIR, "brdf_lut.dds"), LUT_SIZE, LUT_SIZE, 1,
              DXGI_R16G16_FLOAT, False, lut.astype(np.float16).tobytes())

    print("done ->", os.path.abspath(OUT_DIR))


if __name__ == "__main__":
    main()
