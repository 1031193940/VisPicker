#include <doctest/doctest.h>
#include "recording/FrameChange.h"
#include "recording/GifPreview.h"
#include "recording/RecordingFiles.h"
#include "gif/Timing.h"
#include "platform/FileCommit.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>

namespace {
class RecordingTestDirectory final {
public:
    RecordingTestDirectory() {
        path = std::filesystem::temp_directory_path()/(L"VisPick-recording-files-"+
            std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        if (!std::filesystem::create_directory(path)) { throw std::runtime_error("Test directory already exists"); }
    }
    ~RecordingTestDirectory() { std::error_code ignored; std::filesystem::remove_all(path,ignored); }
    RecordingTestDirectory(const RecordingTestDirectory&) = delete;
    RecordingTestDirectory& operator=(const RecordingTestDirectory&) = delete;
    std::filesystem::path path;
};
std::string Contents(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
}

TEST_CASE("Recording memory plans include workers in flight and adapt to architecture") {
    for (const unsigned bits : {32U, 64U}) {
        for (const unsigned processors : {1U, 2U, 4U, 64U}) {
            const auto plan = vispick::recording::PlanMemory(1920, 1080, 1920, 1080, processors, bits);
            CHECK(plan.workerCount >= 1);
            CHECK(plan.workerCount <= 6);
            CHECK(plan.slotCount >= 2);
            CHECK(plan.slotCount <= 8);
            CHECK(plan.maximumBytes <= plan.memoryLimit);
            CHECK(plan.memoryLimit == (bits == 32 ? 128ULL : 256ULL) * 1024 * 1024);
        }
    }
    CHECK_THROWS_AS(static_cast<void>(vispick::recording::PlanMemory(
        std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), 65535, 65535, 6)), std::length_error);
    CHECK_THROWS_AS(static_cast<void>(vispick::recording::PlanMemory(100, 100, 100, 100, 4, 16)), std::invalid_argument);
}

TEST_CASE("Recording changes ignore capture alpha and include boundary pixels") {
    vispick::Bitmap previous(4, 3);
    std::fill(previous.Pixels().begin(), previous.Pixels().end(), 0xFF123456U);
    auto current = previous;
    current.Pixels()[0] = 0x00123456;
    CHECK_FALSE(vispick::recording::ChangedRectangle(current.View(), previous.View()).has_value());
    current.Row(1)[2] = 0xFF987654;
    current.Row(2)[3] = 0xFF000001;
    const auto changed = vispick::recording::ChangedRectangle(current.View(), previous.View());
    REQUIRE(changed.has_value());
    CHECK(changed->left == 2);
    CHECK(changed->top == 1);
    CHECK(changed->right == 4);
    CHECK(changed->bottom == 3);
    std::vector<std::uint32_t> crop;
    vispick::recording::CopyRectangle(current.View(), *changed, crop);
    CHECK(crop == std::vector<std::uint32_t>{0xFF987654, 0xFF123456, 0xFF123456, 0xFF000001});
}

TEST_CASE("Recording delta rejects empty mismatched and unchanged work") {
    vispick::Bitmap image(2, 2);
    vispick::Bitmap other(3, 2);
    CHECK_THROWS_AS(static_cast<void>(vispick::recording::ChangedRectangle(image.View(), other.View())), std::invalid_argument);
    std::vector<std::uint32_t> crop;
    CHECK_THROWS_AS(vispick::recording::CopyRectangle(image.View(), {-1,0,2,2}, crop), std::invalid_argument);
    vispick::recording::RegionEncoder encoder;
    CHECK_THROWS_AS(static_cast<void>(encoder.Encode(image.Pixels(), image.Pixels(), 2, false)), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(encoder.Encode(image.Pixels(), {}, 3, false)), std::invalid_argument);
}

TEST_CASE("Recording transparent deltas preserve exact WIC-composed UI pixels") {
    const auto path = std::filesystem::temp_directory_path() /
        (L"VisPick-recording-delta-" + std::to_wstring(GetCurrentProcessId()) + L".gif");
    struct RemoveFile { std::filesystem::path path; ~RemoveFile() { std::error_code ignored; std::filesystem::remove(path, ignored); } } cleanup{path};
    vispick::Bitmap first(257, 2);
    for (std::size_t pixel = 0; pixel < first.Pixels().size(); ++pixel) {
        first.Pixels()[pixel] = 0xFF000000U | static_cast<std::uint32_t>(pixel % 255) * 0x010101;
    }
    auto second = first;
    second.Row(0)[0] = 0xFFABCD00;
    second.Row(1)[256] = 0xFF00CDEF;
    const auto rectangle = vispick::recording::ChangedRectangle(second.View(), first.View());
    REQUIRE(rectangle.has_value());
    std::vector<std::uint32_t> current, previous;
    vispick::recording::CopyRectangle(second.View(), *rectangle, current);
    vispick::recording::CopyRectangle(first.View(), *rectangle, previous);
    vispick::recording::RegionEncoder encoder;
    const auto initial = encoder.Encode(first.Pixels(), {}, first.Width(), true);
    const auto delta = encoder.Encode(current, previous, second.Width(), true);
    CHECK(initial.frame.exact);
    CHECK(initial.transparentColorIndex == -1);
    CHECK(delta.frame.exact);
    CHECK(delta.frame.palette.size() == 3); // Two changed colors plus transparency.
    REQUIRE(delta.transparentColorIndex >= 0);
    {
        vispick::gif::GifWriter writer(path, first.Width(), first.Height());
        writer.WriteFrame(initial.frame, 3);
        writer.WriteFrame(delta.frame, {0,0,257,2,7,vispick::gif::DisposalMethod::Keep,delta.transparentColorIndex});
        writer.Finish();
    }
    const std::array expected{vispick::gif::PixelHash(first.Pixels()), vispick::gif::PixelHash(second.Pixels())};
    const auto decoded = vispick::gif::InspectGif(path, expected);
    CHECK(decoded.frameCount == 2);
    CHECK(decoded.durationCentiseconds == 10);
    CHECK(decoded.mismatchedFrames == 0);
}

TEST_CASE("Recording keeps a short final image with browser compatible timing and palette limits") {
    RecordingTestDirectory directory;
    for (const unsigned opaqueColors : {1U, 255U}) {
        const auto path = directory.path / (std::to_wstring(opaqueColors) + L"-colors.gif");
        vispick::Bitmap first(256,1);
        std::fill(first.Pixels().begin(),first.Pixels().end(),0xFF000000U);
        auto finalImage = first;
        for (unsigned column = 0; column < 256; ++column) {
            if (column != 128) { finalImage.Pixels()[column] |= opaqueColors == 1 ? 1U : (column < 128 ? column+1 : column); }
        }
        vispick::recording::RegionEncoder encoder;
        const auto initial = encoder.Encode(first.Pixels(),{},first.Width(),false);
        const auto delta = encoder.Encode(finalImage.Pixels(),first.Pixels(),first.Width(),false);
        REQUIRE(delta.frame.palette.size() == opaqueColors + 1);
        REQUIRE(delta.transparentColorIndex >= 0);
        {
            vispick::gif::TimestampDelayAccumulator times(100000000);
            vispick::gif::GifWriter writer(path,256,1);
            writer.WriteFrame(initial.frame,vispick::recording::RecordedFrameDelay(times.AdvanceTo(132000000)));
            writer.WriteFrame(delta.frame,{0,0,256,1,vispick::recording::RecordedFrameDelay(times.AdvanceTo(133000000)),
                vispick::gif::DisposalMethod::Keep,delta.transparentColorIndex});
            writer.Finish();
        }
        const std::array expected{vispick::gif::PixelHash(first.Pixels()),vispick::gif::PixelHash(finalImage.Pixels())};
        const auto decoded = vispick::gif::InspectGif(path,expected);
        CHECK(decoded.frameCount == 2);
        CHECK(decoded.mismatchedFrames == 0);
        CHECK(decoded.durationCentiseconds == 5);
        vispick::gif::GifReader reader(path);
        CHECK(reader.ReadFrame(1).metadata.delayCentiseconds == 2);
    }
}

TEST_CASE("Failed GIF preview construction releases its owned recording") {
    RecordingTestDirectory directory;
    vispick::RecordingResult recording;
    recording.path = directory.path / L"owned-invalid.gif";
    const auto temporary = recording.path;
    { std::ofstream stream(temporary); stream << "owned recording"; }
    CHECK_THROWS_AS(vispick::GifPreview(std::move(recording),{},nullptr,WM_APP + 1,{}),std::invalid_argument);
    CHECK_FALSE(std::filesystem::exists(temporary));
}

TEST_CASE("Floyd Steinberg preserves average tone with bounded deterministic scratch") {
    const std::vector<std::uint32_t> gray(64 * 64, 0xFF808080);
    vispick::gif::IndexedImage image{{0xFF000000,0xFFFFFFFF},std::vector<std::uint8_t>(gray.size()),false,false,0};
    auto repeated = image;
    vispick::gif::DitherFloydSteinberg(gray, 64, image);
    vispick::gif::DitherFloydSteinberg(gray, 64, repeated);
    CHECK(image.pixels == repeated.pixels);
    const auto whitePixels = std::count(image.pixels.begin(), image.pixels.end(), 1);
    const double average = static_cast<double>(whitePixels) / static_cast<double>(gray.size()) * 255;
    CHECK(average > 126);
    CHECK(average < 130);
    CHECK(image.maximumChannelError == 128);
    CHECK(std::all_of(image.pixels.begin(), image.pixels.end(), [](std::uint8_t value) { return value <= 1; }));
}

TEST_CASE("Dither cache distinguishes nearby colors on opposite sides of a palette boundary") {
    const std::array pixels{0xFF646464U,0xFF676767U};
    vispick::gif::IndexedImage image{{pixels[0],pixels[1]},{0,0},false,false,0};
    vispick::gif::DitherFloydSteinberg(pixels, 2, image);
    CHECK(image.pixels == std::vector<std::uint8_t>{0,1});
    CHECK(image.exact);
}

TEST_CASE("Floyd Steinberg respects transparent holes and never alters an exact palette") {
    const std::vector<std::uint32_t> pixels(16, 0xFF808080);
    vispick::gif::IndexedImage lossy{{0xFF000000,0xFFFFFFFF,0},std::vector<std::uint8_t>(16),false,false,0};
    lossy.pixels[5] = 2;
    lossy.pixels[6] = 2;
    vispick::gif::DitherFloydSteinberg(pixels, 4, lossy, 2);
    CHECK(lossy.pixels[5] == 2);
    CHECK(lossy.pixels[6] == 2);
    auto exact = vispick::gif::Quantize(pixels);
    const auto before = exact;
    vispick::gif::DitherFloydSteinberg(pixels, 4, exact);
    CHECK(exact.palette == before.palette);
    CHECK(exact.pixels == before.pixels);
    CHECK_THROWS_AS(vispick::gif::DitherFloydSteinberg(pixels, 0, lossy), std::invalid_argument);
}

TEST_CASE("Pause removes only paused wall time from actual capture timestamps") {
    vispick::recording::ActiveTimeline timeline;
    CHECK(timeline.Elapsed(100000000) == 100000000);
    timeline.Pause(100000000, true);
    CHECK(timeline.Elapsed(900000000) == 100000000);
    timeline.Pause(1100000000, false);
    CHECK(timeline.Elapsed(1200000000) == 200000000);
    timeline.Pause(1300000000, true);
    timeline.Pause(1500000000, false);
    CHECK(timeline.Elapsed(1600000000) == 400000000);
    vispick::gif::TimestampDelayAccumulator delays(0);
    CHECK(delays.AdvanceTo(timeline.Elapsed(1600000000)) == 40);
    CHECK_THROWS_AS(static_cast<void>(timeline.Elapsed(1400000000)), std::invalid_argument);
}

TEST_CASE("A static five-minute recording fits in one GIF frame delay") {
    vispick::gif::TimestampDelayAccumulator delays(0);
    CHECK(vispick::recording::RecordedFrameDelay(delays.AdvanceTo(300000000000LL)) == 30000);
}

TEST_CASE("GIF save commits replacement and preserves the source when destination fails") {
    RecordingTestDirectory directory;
    const auto source = directory.path/L"recording.gif";
    const auto destination = directory.path/L"saved.gif";
    { std::ofstream stream(source); stream << "new GIF bytes"; }
    { std::ofstream stream(destination); stream << "previous saved content"; }
    CHECK_THROWS_AS(vispick::recording::CommitFile(source,source),std::invalid_argument);
    CHECK(Contents(source) == "new GIF bytes");
    CHECK_THROWS(vispick::recording::CommitFile(source,directory.path/L"missing-parent"/L"save.gif"));
    CHECK(Contents(source) == "new GIF bytes");
    CHECK(Contents(destination) == "previous saved content");
    REQUIRE(SetFileAttributesW(destination.c_str(),FILE_ATTRIBUTE_READONLY) != FALSE);
    CHECK_THROWS(vispick::recording::CommitFile(source,destination));
    CHECK(SetFileAttributesW(destination.c_str(),FILE_ATTRIBUTE_NORMAL) != FALSE);
    CHECK(Contents(source) == "new GIF bytes");
    CHECK(Contents(destination) == "previous saved content");
    vispick::recording::CommitFile(source,destination);
    CHECK_FALSE(std::filesystem::exists(source));
    CHECK(Contents(destination) == "new GIF bytes");
}

TEST_CASE("GIF overwrite accepts a Unicode destination beyond MAX_PATH") {
    RecordingTestDirectory directory;
    const auto source = directory.path / L"recording.gif";
    const auto nested = directory.path / std::wstring(80,L'长') / std::wstring(80,L'路') / std::wstring(80,L'径');
    const auto destination = nested / L"已保存.gif";
    REQUIRE(destination.native().size() > MAX_PATH);
    const auto extended = vispick::ExtendedFilePath(destination);
    const auto extendedRoot = vispick::ExtendedFilePath(directory.path);
    struct RemoveExtendedTree {
        const std::filesystem::path& path;
        ~RemoveExtendedTree() { std::error_code ignored; std::filesystem::remove_all(path,ignored); }
    } cleanup{extendedRoot};
    std::filesystem::create_directories(extended.parent_path());
    { std::ofstream stream(source); stream << "replacement"; }
    { std::ofstream stream(extended); stream << "original"; }
    vispick::recording::CommitFile(source,destination);
    CHECK_FALSE(std::filesystem::exists(source));
    CHECK(Contents(extended) == "replacement");
    CHECK(std::distance(std::filesystem::directory_iterator(extended.parent_path()),std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("Old recording cleanup only removes expired files with the exact owned pattern") {
    RecordingTestDirectory directory;
    const auto now = std::filesystem::file_time_type::clock::now();
    const auto old = now-std::chrono::hours(48);
    for (const auto* name : {L"rec_vispick_1_2_3.gif",L"rec_vispick_1_2_4.gif",L"rec_vispick_1__3.gif",
                            L"rec_vispick_name_2_3.gif",L"rec_other_1_2_3.gif",L"user.gif"}) {
        const auto path = directory.path/name;
        { std::ofstream stream(path); stream << "preserve unless exact old recording"; }
        std::filesystem::last_write_time(path,name == std::wstring_view(L"rec_vispick_1_2_4.gif") ? now : old);
    }
    std::filesystem::create_directory(directory.path/L"rec_vispick_5_6_7.gif");
    std::filesystem::last_write_time(directory.path/L"rec_vispick_5_6_7.gif",old);
    vispick::recording::CleanOldFiles(directory.path,now-std::chrono::hours(24));
    CHECK_FALSE(std::filesystem::exists(directory.path/L"rec_vispick_1_2_3.gif"));
    for (const auto* name : {L"rec_vispick_1_2_4.gif",L"rec_vispick_1__3.gif",L"rec_vispick_name_2_3.gif",
                            L"rec_other_1_2_3.gif",L"user.gif",L"rec_vispick_5_6_7.gif"}) {
        CHECK(std::filesystem::exists(directory.path/name));
    }
}
