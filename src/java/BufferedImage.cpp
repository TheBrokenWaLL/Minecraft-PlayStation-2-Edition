#include "java/BufferedImage.h"

#include <cassert>
#include <limits>
#include <stdexcept>
#include <cstring>

#include "stb_image.h"

std::size_t BufferedImage::checkedPixelCount(int_t width, int_t height)
{
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("BufferedImage: non-positive dimensions");

    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    if (w != 0 && h > std::numeric_limits<std::size_t>::max() / w)
        throw std::overflow_error("BufferedImage: pixel count overflow");
    return w * h;
}

std::size_t BufferedImage::checkedRgbaByteCount(int_t width, int_t height)
{
    const std::size_t pixels = checkedPixelCount(width, height);
    if (pixels > std::numeric_limits<std::size_t>::max() / 4u)
        throw std::overflow_error("BufferedImage: RGBA byte count overflow");
    return pixels * 4u;
}

BufferedImage::BufferedImage(int_t width, int_t height, std::unique_ptr<unsigned char[]> raw_pixels)
{
    const std::size_t byteCount = checkedRgbaByteCount(width, height);
    if (byteCount != 0 && raw_pixels == nullptr)
        throw std::invalid_argument("BufferedImage: null pixel buffer");
    this->width = width;
    this->height = height;
    this->raw_pixels = std::move(raw_pixels);
}

BufferedImage::BufferedImage(int_t width, int_t height)
{
    this->width = width;
    this->height = height;
    this->raw_pixels = Util::make_unique<unsigned char[]>(checkedRgbaByteCount(width, height));
}

int_t BufferedImage::getWidth() const
{
    return width;
}

int_t BufferedImage::getHeight() const
{
    return height;
}

const unsigned char *BufferedImage::getRawPixels() const
{
    return raw_pixels.get();
}

void BufferedImage::validateRegion(int_t startX, int_t startY, int_t w, int_t h) const
{
    if (startX < 0 || startY < 0 || w < 0 || h < 0)
        throw std::out_of_range("BufferedImage: negative image region");

    const std::int64_t endX = static_cast<std::int64_t>(startX) + static_cast<std::int64_t>(w);
    const std::int64_t endY = static_cast<std::int64_t>(startY) + static_cast<std::int64_t>(h);
    if (endX > static_cast<std::int64_t>(width) || endY > static_cast<std::int64_t>(height))
        throw std::out_of_range("BufferedImage: image region outside bounds");
}

void BufferedImage::getRGB(int_t startX, int_t startY, int_t w, int_t h, unsigned char *rgbArray) const
{
    validateRegion(startX, startY, w, h);
    const std::size_t regionBytes = checkedRgbaByteCount(w, h);
    if (regionBytes != 0 && rgbArray == nullptr)
        throw std::invalid_argument("BufferedImage::getRGB: null output buffer");

    if (w == 0 || h == 0)
        return;

    const std::size_t imageWidth = static_cast<std::size_t>(width);
    const std::size_t regionWidth = static_cast<std::size_t>(w);
    const std::size_t rowBytes = regionWidth * 4u;

    // Ruta ultra rápida: copia de la imagen completa en un único bloque de memoria
    if (startX == 0 && w == width && startY == 0 && h == height)
    {
        std::memcpy(rgbArray, raw_pixels.get(), regionBytes);
        return;
    }

    // Copia fila por fila con memcpy en vez de leer byte a byte en CPU
    for (int_t y = 0; y < h; y++)
    {
        const std::size_t srcY = static_cast<std::size_t>(startY) + static_cast<std::size_t>(y);
        const std::size_t srcIndex = (srcY * imageWidth + static_cast<std::size_t>(startX)) * 4u;
        const std::size_t dstIndex = static_cast<std::size_t>(y) * rowBytes;

        std::memcpy(&rgbArray[dstIndex], &raw_pixels[srcIndex], rowBytes);
    }
}

void BufferedImage::setRGB(int_t startX, int_t startY, int_t w, int_t h, unsigned char *rgbArray)
{
    validateRegion(startX, startY, w, h);
    const std::size_t regionBytes = checkedRgbaByteCount(w, h);
    if (regionBytes != 0 && rgbArray == nullptr)
        throw std::invalid_argument("BufferedImage::setRGB: null input buffer");

    if (w == 0 || h == 0)
        return;

    const std::size_t imageWidth = static_cast<std::size_t>(width);
    const std::size_t regionWidth = static_cast<std::size_t>(w);
    const std::size_t rowBytes = regionWidth * 4u;

    // Ruta ultra rápida: utilizada por ReiMinimap cada vez que sube los 64x64 píxeles completos
    if (startX == 0 && w == width && startY == 0 && h == height)
    {
        std::memcpy(raw_pixels.get(), rgbArray, regionBytes);
        return;
    }

    // Copia fila por fila con memcpy eliminando decenas de miles de operaciones por byte
    for (int_t y = 0; y < h; y++)
    {
        const std::size_t srcIndex = static_cast<std::size_t>(y) * rowBytes;
        const std::size_t dstY = static_cast<std::size_t>(startY) + static_cast<std::size_t>(y);
        const std::size_t dstIndex = (dstY * imageWidth + static_cast<std::size_t>(startX)) * 4u;

        std::memcpy(&raw_pixels[dstIndex], &rgbArray[srcIndex], rowBytes);
    }
}

static int istream_read(void *user, char *data, int size)
{
    auto &in = *reinterpret_cast<std::istream *>(user);
    in.read(reinterpret_cast<char *>(data), size);
    return static_cast<int>(in.gcount());
}

static void istream_skip(void *user, int n)
{
    auto &in = *reinterpret_cast<std::istream *>(user);
    in.seekg(n, std::ios::cur);
}

static int istream_eof(void *user)
{
    auto &in = *reinterpret_cast<std::istream *>(user);
    return in.eof();
}

stbi_io_callbacks stbi_io_callbacks_istream = { istream_read, istream_skip, istream_eof };

BufferedImage BufferedImage::ImageIO_read(std::istream &in)
{
    // Solicitar directamente 4 canales (STBI_rgb_alpha).
    // stb_image convierte internamente 1, 2, 3 o 4 canales a RGBA de 32 bits de forma nativa.
    int w = 0, h = 0, comp = 0;
    stbi_uc *raw_data = stbi_load_from_callbacks(&stbi_io_callbacks_istream, &in, &w, &h, &comp, STBI_rgb_alpha);

    if (raw_data == nullptr)
        throw std::runtime_error(std::string("ImageIO_read: decode failed: ") +
                                 (stbi_failure_reason() ? stbi_failure_reason() : "unknown"));

    const std::size_t byteCount = checkedRgbaByteCount(w, h);
    std::unique_ptr<unsigned char[]> data = Util::make_unique<unsigned char[]>(byteCount);

    // Copia directa en un solo bloque con std::memcpy sin bucles C++ manuales
    std::memcpy(data.get(), raw_data, byteCount);
    stbi_image_free(raw_data);

    return BufferedImage(w, h, std::move(data));
};