#include "editor/Renderer.h"
#include "platform/Gdi.h"
#include "platform/GdiPlus.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace vispick {
namespace {

void CheckGdiPlus(Gdiplus::Status status) {
    if (status != Gdiplus::Ok) {
        throw std::runtime_error("GDI+ annotation operation failed: " + std::to_string(status));
    }
}

class SavedDc {
public:
    explicit SavedDc(HDC deviceContext) : deviceContext_(deviceContext), saved_(SaveDC(deviceContext)) {
        if (saved_ == 0) { throw std::runtime_error("SaveDC failed"); }
    }
    ~SavedDc() { RestoreDC(deviceContext_, saved_); }
    SavedDc(const SavedDc&) = delete;
    SavedDc& operator=(const SavedDc&) = delete;

private:
    HDC deviceContext_;
    int saved_;
};

RECT LocalRectangle(RECT screen, POINT origin) noexcept {
    return {screen.left - origin.x, screen.top - origin.y, screen.right - origin.x, screen.bottom - origin.y};
}

Gdiplus::Color ShapeColor(std::uint32_t color) {
    return Gdiplus::Color(255, static_cast<BYTE>(color >> 16), static_cast<BYTE>(color >> 8), static_cast<BYTE>(color));
}

void DrawAnnotation(HDC destination, POINT origin, RECT screenClip, const Annotation& annotation,
                    std::vector<Gdiplus::PointF>& curvePoints) {
    RECT intersection{};
    if (!IntersectRect(&intersection, &screenClip, &annotation.bounds)) { return; }
    const SavedDc saved(destination);
    const auto localClip = LocalRectangle(intersection, origin);
    IntersectClipRect(destination, localClip.left, localClip.top, localClip.right, localClip.bottom);
    if (annotation.tool == AnnotationTool::Mosaic && annotation.mosaic) {
        const auto& bitmap = *annotation.mosaic;
        BITMAPINFO information{};
        information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        information.bmiHeader.biWidth = bitmap.Width();
        information.bmiHeader.biHeight = -bitmap.Height();
        information.bmiHeader.biPlanes = 1;
        information.bmiHeader.biBitCount = 32;
        information.bmiHeader.biCompression = BI_RGB;
        if (StretchDIBits(destination, annotation.bounds.left - origin.x, annotation.bounds.top - origin.y,
                          bitmap.Width(), bitmap.Height(), 0, 0, bitmap.Width(), bitmap.Height(),
                          bitmap.Pixels().data(), &information, DIB_RGB_COLORS, SRCCOPY) == GDI_ERROR) {
            throw std::runtime_error("Mosaic blit failed");
        }
        return;
    }
    if (annotation.tool == AnnotationTool::Text) {
        const UniqueGdiObject<HFONT> font(CreateFontIndirectW(&annotation.font));
        if (font.Get() == nullptr) { throw std::runtime_error("Text annotation font creation failed"); }
        const SelectObjectGuard selected(destination, font.Get());
        SetBkMode(destination, TRANSPARENT);
        SetTextAlign(destination, TA_LEFT | TA_TOP | TA_NOUPDATECP);
        SetTextColor(destination,
                     RGB((annotation.color >> 16) & 0xFF, (annotation.color >> 8) & 0xFF, annotation.color & 0xFF));
        TEXTMETRICW metrics{};
        if (!GetTextMetricsW(destination, &metrics)) { throw std::runtime_error("Text annotation metrics failed"); }
        const int tabWidth = std::max(1L, metrics.tmAveCharWidth * 8);
        const int textOrigin = annotation.bounds.left - origin.x;
        for (const auto& line : annotation.textLines) {
            if (line.text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                throw std::length_error("Text annotation line too long");
            }
            if (!line.text.empty()) {
                const int textTop = annotation.bounds.top - origin.y + line.offset.y;
                if (line.tabOffsets.empty()) {
                    TabbedTextOutW(destination, textOrigin + line.offset.x, textTop, line.text.c_str(),
                                   static_cast<int>(line.text.size()), 1, &tabWidth, textOrigin);
                } else {
                    std::size_t begin = 0;
                    std::size_t tab = 0;
                    LONG horizontal = line.offset.x;
                    while (begin < line.text.size()) {
                        const auto end = line.text.find(L'\t', begin);
                        const auto length = (end == std::wstring::npos ? line.text.size() : end) - begin;
                        if (length != 0) {
                            TextOutW(destination, textOrigin + horizontal, textTop, line.text.data() + begin,
                                     static_cast<int>(length));
                        }
                        if (end == std::wstring::npos) { break; }
                        if (tab >= line.tabOffsets.size()) {
                            throw std::invalid_argument("Missing native text tab position");
                        }
                        horizontal = line.tabOffsets[tab++];
                        begin = end + 1;
                    }
                }
            }
        }
        return;
    }
    if (annotation.points.empty()) { return; }
    if (!EnsureGdiPlus()) { throw std::runtime_error("GDI+ initialization failed"); }
    Gdiplus::Graphics graphics(destination);
    CheckGdiPlus(graphics.GetLastStatus());
    CheckGdiPlus(graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias));
    CheckGdiPlus(graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf));
    CheckGdiPlus(graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver));
    Gdiplus::Pen pen(ShapeColor(annotation.color), static_cast<Gdiplus::REAL>(annotation.strokePixels));
    CheckGdiPlus(pen.SetLineJoin(Gdiplus::LineJoinRound));
    CheckGdiPlus(pen.SetStartCap(Gdiplus::LineCapRound));
    CheckGdiPlus(pen.SetEndCap(Gdiplus::LineCapRound));
    const auto convert = [origin](POINT point) {
        return Gdiplus::PointF(static_cast<float>(point.x - origin.x), static_cast<float>(point.y - origin.y));
    };
    const auto start = convert(annotation.points.front());
    const auto end = convert(annotation.points.back());
    const Gdiplus::RectF rectangle(std::min(start.X, end.X), std::min(start.Y, end.Y), std::abs(end.X - start.X),
                                   std::abs(end.Y - start.Y));
    if (annotation.tool == AnnotationTool::Rectangle || annotation.tool == AnnotationTool::Mosaic) {
        CheckGdiPlus(graphics.DrawRectangle(&pen, rectangle));
    } else if (annotation.tool == AnnotationTool::Ellipse) {
        CheckGdiPlus(graphics.DrawEllipse(&pen, rectangle));
    } else if (annotation.tool == AnnotationTool::Arrow) {
        const float horizontal = end.X - start.X;
        const float vertical = end.Y - start.Y;
        const float length = std::hypot(horizontal, vertical);
        if (length <= 0) { return; }
        const float headLength = std::min(length * 0.65F, std::max(8.0F, annotation.strokePixels * 4.0F));
        const float directionX = horizontal / length;
        const float directionY = vertical / length;
        const Gdiplus::PointF base(end.X - directionX * headLength, end.Y - directionY * headLength);
        const Gdiplus::PointF corners[]{
            end,
            {base.X - directionY * headLength * 0.5F, base.Y + directionX * headLength * 0.5F},
            {base.X + directionY * headLength * 0.5F, base.Y - directionX * headLength * 0.5F}};
        CheckGdiPlus(graphics.DrawLine(&pen, start, base));
        Gdiplus::SolidBrush brush(ShapeColor(annotation.color));
        CheckGdiPlus(graphics.FillPolygon(&brush, corners, 3));
    } else if (annotation.tool == AnnotationTool::Pen) {
        if (annotation.points.size() == 1) {
            const float size = static_cast<float>(annotation.strokePixels);
            Gdiplus::SolidBrush brush(ShapeColor(annotation.color));
            CheckGdiPlus(graphics.FillEllipse(&brush, start.X - size * 0.5F, start.Y - size * 0.5F, size, size));
        } else if (annotation.points.size() == 2) {
            CheckGdiPlus(graphics.DrawLine(&pen, start, end));
        } else {
            curvePoints.clear();
            if (annotation.points.size() > static_cast<std::size_t>((std::numeric_limits<int>::max() - 1) / 3)) {
                throw std::length_error("Annotation curve too long");
            }
            curvePoints.reserve(annotation.points.size() * 3);
            // Quadratic corner smoothing converted to cubic Beziers stays inside the
            // samples' convex hull. Cardinal splines can overshoot the dirty rectangle.
            curvePoints.push_back(start);
            Gdiplus::PointF previous = start;
            for (std::size_t index = 1; index + 1 < annotation.points.size(); ++index) {
                const auto control = convert(annotation.points[index]);
                const auto next = convert(annotation.points[index + 1]);
                const Gdiplus::PointF middle((control.X + next.X) * 0.5F, (control.Y + next.Y) * 0.5F);
                curvePoints.push_back({previous.X + (control.X - previous.X) * (2.0F / 3.0F),
                                       previous.Y + (control.Y - previous.Y) * (2.0F / 3.0F)});
                curvePoints.push_back({middle.X + (control.X - middle.X) * (2.0F / 3.0F),
                                       middle.Y + (control.Y - middle.Y) * (2.0F / 3.0F)});
                curvePoints.push_back(middle);
                previous = middle;
            }
            curvePoints.push_back({previous.X + (end.X - previous.X) / 3.0F, previous.Y + (end.Y - previous.Y) / 3.0F});
            curvePoints.push_back(
                {previous.X + (end.X - previous.X) * (2.0F / 3.0F), previous.Y + (end.Y - previous.Y) * (2.0F / 3.0F)});
            curvePoints.push_back(end);
            if (curvePoints.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                throw std::length_error("Annotation curve too long");
            }
            CheckGdiPlus(graphics.DrawBeziers(&pen, curvePoints.data(), static_cast<int>(curvePoints.size())));
        }
    }
}

} // namespace

struct EditorRenderer::State {
    explicit State(const AnnotationModel& source) : model(source) {}
    void EnsureCache() {
        if (revision == model.Revision()) { return; }
        RECT annotationRegion{};
        const auto annotations = model.Committed();
        for (std::size_t annotationIndex = 0; annotationIndex < annotations.size(); ++annotationIndex) {
            if (hiddenAnnotation == annotationIndex) { continue; }
            UnionRect(&annotationRegion, &annotationRegion, &annotations[annotationIndex].bounds);
        }
        const auto desktop = model.DesktopBounds();
        IntersectRect(&cacheBounds, &annotationRegion, &desktop);
        const int width = cacheBounds.right - cacheBounds.left;
        const int height = cacheBounds.bottom - cacheBounds.top;
        if (cache) { GdiFlush(); } // Complete prior GDI reads before CPU overwrites the cache.
        constexpr std::uint64_t maximumCacheBytes = 16ULL * 1024 * 1024;
        if (width <= 0 || height <= 0 ||
            static_cast<std::uint64_t>(width) * height * sizeof(std::uint32_t) > maximumCacheBytes) {
            cache.reset();
            revision = model.Revision();
            return;
        }
        if (!cache || cache->Width() != width || cache->Height() != height) {
            cache.reset();
            cache = std::make_unique<DibSection>(width, height);
        }
        const auto frozen = model.Frozen();
        auto pixels = cache->Pixels();
        for (int row = 0; row < height; ++row) {
            const auto* source = frozen.Row(cacheBounds.top - desktop.top + row) + (cacheBounds.left - desktop.left);
            auto* target = pixels.data() + static_cast<std::size_t>(row) * width;
            std::transform(source, source + width, target, [](std::uint32_t pixel) { return pixel | 0xFF000000U; });
        }
        for (std::size_t annotationIndex = 0; annotationIndex < annotations.size(); ++annotationIndex) {
            if (hiddenAnnotation == annotationIndex) { continue; }
            DrawAnnotation(cache->DeviceContext(), {cacheBounds.left, cacheBounds.top}, cacheBounds,
                           annotations[annotationIndex], curvePoints);
        }
        GdiFlush();
        // GDI text does not preserve the DIB alpha byte. This combined cache is explicitly
        // opaque, hence also valid premultiplied BGRA; antialiasing was blended over real pixels.
        for (auto& pixel : pixels) { pixel |= 0xFF000000U; }
        revision = model.Revision();
        ++buildCount;
    }
    const AnnotationModel& model;
    RECT cacheBounds{};
    std::unique_ptr<DibSection> cache;
    std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t buildCount = 0;
    std::optional<std::size_t> hiddenAnnotation;
    std::vector<Gdiplus::PointF> curvePoints;
};

EditorRenderer::EditorRenderer(const AnnotationModel& model) : state_(std::make_unique<State>(model)) {}
EditorRenderer::~EditorRenderer() = default;

void EditorRenderer::Draw(HDC destination, POINT destinationScreenOrigin, RECT screenClip, bool preview) {
    RECT clipped{};
    const auto desktop = state_->model.DesktopBounds();
    if (!IntersectRect(&clipped, &screenClip, &desktop)) { return; }
    state_->EnsureCache();
    RECT cached{};
    const bool hasCachedPixels = state_->cache && IntersectRect(&cached, &clipped, &state_->cacheBounds);
    if (!hasCachedPixels || !EqualRect(&cached, &clipped)) {
        const auto frozen = state_->model.Frozen();
        BITMAPINFO information{};
        information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        information.bmiHeader.biWidth = frozen.Width();
        information.bmiHeader.biHeight = -frozen.Height();
        information.bmiHeader.biPlanes = 1;
        information.bmiHeader.biBitCount = 32;
        information.bmiHeader.biCompression = BI_RGB;
        if (StretchDIBits(destination, clipped.left - destinationScreenOrigin.x,
                          clipped.top - destinationScreenOrigin.y, clipped.right - clipped.left,
                          clipped.bottom - clipped.top, clipped.left - desktop.left,
                          frozen.Height() - (clipped.bottom - desktop.top), clipped.right - clipped.left,
                          clipped.bottom - clipped.top, frozen.Pixels().data(), &information, DIB_RGB_COLORS,
                          SRCCOPY) == GDI_ERROR) {
            throw std::runtime_error("Frozen editor source blit failed");
        }
    }
    if (hasCachedPixels) {
        if (!BitBlt(destination, cached.left - destinationScreenOrigin.x, cached.top - destinationScreenOrigin.y,
                    cached.right - cached.left, cached.bottom - cached.top, state_->cache->DeviceContext(),
                    cached.left - state_->cacheBounds.left, cached.top - state_->cacheBounds.top, SRCCOPY)) {
            throw std::runtime_error("Annotation cache blit failed");
        }
    } else if (!state_->cache) {
        const auto annotations = state_->model.Committed();
        for (std::size_t annotationIndex = 0; annotationIndex < annotations.size(); ++annotationIndex) {
            if (state_->hiddenAnnotation == annotationIndex) { continue; }
            DrawAnnotation(destination, destinationScreenOrigin, clipped, annotations[annotationIndex],
                           state_->curvePoints);
        }
    }
    if (preview && state_->model.Preview() != nullptr) {
        DrawAnnotation(destination, destinationScreenOrigin, clipped, *state_->model.Preview(), state_->curvePoints);
    }
}

Bitmap EditorRenderer::Flatten(RECT screenSelection) {
    RECT clipped{};
    const auto desktop = state_->model.DesktopBounds();
    if (!IntersectRect(&clipped, &screenSelection, &desktop)) {
        throw std::invalid_argument("Empty annotation output selection");
    }
    state_->EnsureCache();
    Bitmap output = state_->model.Frozen().CropOpaque(clipped.left - desktop.left, clipped.top - desktop.top,
                                                      clipped.right - clipped.left, clipped.bottom - clipped.top);
    if (state_->cache) {
        RECT overlap{};
        if (IntersectRect(&overlap, &clipped, &state_->cacheBounds)) {
            const BitmapView view(state_->cache->Width(), state_->cache->Height(), state_->cache->Pixels());
            for (LONG row = overlap.top; row < overlap.bottom; ++row) {
                const auto* source =
                    view.Row(row - state_->cacheBounds.top) + (overlap.left - state_->cacheBounds.left);
                std::copy_n(source, overlap.right - overlap.left,
                            output.Row(row - clipped.top) + (overlap.left - clipped.left));
            }
        }
    } else if (state_->model.HasAnnotations()) {
        // For a large annotation region, flatten into a bounded scratch DIB.
        // Copy only tiles that contain annotations over the frozen output.
        constexpr int tileExtent = 512;
        DibSection tile(std::min(tileExtent, output.Width()), std::min(tileExtent, output.Height()));
        const BitmapView view(tile.Width(), tile.Height(), tile.Pixels());
        for (LONG top = clipped.top; top < clipped.bottom; top += tile.Height()) {
            for (LONG left = clipped.left; left < clipped.right; left += tile.Width()) {
                const RECT tileClip{left, top, std::min(clipped.right, left + tile.Width()),
                                    std::min(clipped.bottom, top + tile.Height())};
                const auto annotations = state_->model.Committed();
                if (!std::any_of(annotations.begin(), annotations.end(), [&](const Annotation& annotation) {
                        const auto annotationIndex = static_cast<std::size_t>(&annotation - annotations.data());
                        if (state_->hiddenAnnotation == annotationIndex) { return false; }
                        RECT overlap{};
                        return IntersectRect(&overlap, &tileClip, &annotation.bounds) != FALSE;
                    })) {
                    continue;
                }
                Draw(tile.DeviceContext(), {left, top}, tileClip, false);
                if (!GdiFlush()) { throw std::runtime_error("Flush annotation export tile failed"); }
                for (LONG row = top; row < tileClip.bottom; ++row) {
                    const auto* source = view.Row(row - top);
                    std::transform(source, source + (tileClip.right - left),
                                   output.Row(row - clipped.top) + (left - clipped.left),
                                   [](std::uint32_t pixel) { return pixel | 0xFF000000U; });
                }
            }
        }
    }
    return output;
}

std::uint64_t EditorRenderer::CacheBuildCount() const noexcept {
    return state_->buildCount;
}

void EditorRenderer::SetHiddenAnnotation(std::optional<std::size_t> annotationIndex) noexcept {
    if (state_->hiddenAnnotation == annotationIndex) { return; }
    state_->hiddenAnnotation = annotationIndex;
    state_->revision = std::numeric_limits<std::uint64_t>::max();
}

} // namespace vispick
