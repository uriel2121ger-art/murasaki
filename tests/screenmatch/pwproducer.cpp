// A test-only PipeWire video producer: one stream "murasaki-test-producer" of 200 x 120 BGRx frames at 10 a second. For the first `--switch-after` ticks the picture is scene A, then scene B (the
// same bright boxes as tst_screenmatchsession). It talks to whatever PipeWire server PIPEWIRE_RUNTIME_DIR / PIPEWIRE_CORE name, which the test points at a private one. Never installed.

#include <pipewire/pipewire.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include <sys/mman.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int kW = 200, kH = 120; // (--size WxH makes it bigger for the cost bench; the boxes are scaled)
int kFps = 10;

struct Box {
    int x, y, w, h, value;
};
const Box kA[] = {{20, 15, 60, 40, 220}, {110, 60, 50, 45, 150}, {40, 80, 30, 25, 90}, {150, 10, 35, 30, 250}};
const Box kB[] = {{100, 20, 70, 30, 200}, {15, 70, 40, 40, 120}, {130, 85, 55, 25, 240}};

struct Data {
    pw_main_loop *loop = nullptr;
    pw_stream *stream = nullptr;
    spa_source *timer = nullptr;
    int tick = 0;
    int switchAfter = 30;
};

void paint(uint8_t *out, int stride, bool a)
{
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const int sx = x * 200 / kW, sy = y * 120 / kH; // (the scene is drawn at 200 x 120 and scaled up)
            int v = 20;
            if (a) {
                for (const Box &b : kA)
                    if (sx >= b.x && sx < b.x + b.w && sy >= b.y && sy < b.y + b.h)
                        v = b.value;
            } else {
                for (const Box &b : kB)
                    if (sx >= b.x && sx < b.x + b.w && sy >= b.y && sy < b.y + b.h)
                        v = b.value;
            }
            uint8_t *p = out + y * stride + x * 4;
            p[0] = p[1] = p[2] = uint8_t(v);
            p[3] = 255;
        }
}

void onProcess(void *userdata)
{
    auto *d = static_cast<Data *>(userdata);
    pw_buffer *b = pw_stream_dequeue_buffer(d->stream);
    if (!b)
        return;
    spa_data &data = b->buffer->datas[0];
    if (data.data && data.data != MAP_FAILED) {
        const int stride = kW * 4;
        paint(static_cast<uint8_t *>(data.data), stride, d->tick < d->switchAfter);
        data.chunk->offset = 0;
        data.chunk->stride = stride;
        data.chunk->size = stride * kH;
    }
    pw_stream_queue_buffer(d->stream, b);
}

void onParamChanged(void *userdata, uint32_t id, const spa_pod *param)
{
    auto *d = static_cast<Data *>(userdata);
    if (!param || id != SPA_PARAM_Format)
        return;
    uint8_t buffer[512];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod *params[1];
    params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
                                                                       SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size, SPA_POD_Int(kW * 4 * kH), SPA_PARAM_BUFFERS_stride,
                                                                       SPA_POD_Int(kW * 4), SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd)));
    pw_stream_update_params(d->stream, params, 1);
}

void onTimer(void *userdata, uint64_t)
{
    auto *d = static_cast<Data *>(userdata);
    ++d->tick;
    pw_stream_trigger_process(d->stream);
}

const pw_stream_events kEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = onProcess;
    e.param_changed = onParamChanged;
    return e;
}();

} // namespace

int main(int argc, char **argv)
{
    Data d;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--switch-after"))
            d.switchAfter = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--fps"))
            kFps = std::max(1, std::min(60, std::atoi(argv[i + 1])));
        else if (!std::strcmp(argv[i], "--size")) {
            int w = 0, h = 0;
            if (std::sscanf(argv[i + 1], "%dx%d", &w, &h) == 2 && w >= 16 && h >= 16 && w <= 7680 && h <= 4320) {
                kW = w;
                kH = h;
            }
        }
    }
    pw_init(&argc, &argv);
    d.loop = pw_main_loop_new(nullptr);
    pw_properties *props = pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_NODE_NAME, "murasaki-test-producer", PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", nullptr);
    d.stream = pw_stream_new_simple(pw_main_loop_get_loop(d.loop), "murasaki-test-producer", props, &kEvents, &d);
    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_rectangle size = SPA_RECTANGLE(uint32_t(kW), uint32_t(kH));
    const spa_fraction rate = SPA_FRACTION(uint32_t(kFps), 1);
    const spa_pod *params[1];
    params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                                                                       SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
                                                                       SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate)));
    if (pw_stream_connect(d.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS), params, 1) < 0) {
        std::fprintf(stderr, "cannot connect the producer\n");
        return 1;
    }
    d.timer = pw_loop_add_timer(pw_main_loop_get_loop(d.loop), onTimer, &d);
    timespec first{0, 200 * 1000 * 1000}, every{kFps == 1 ? 1 : 0, kFps == 1 ? 0 : 1000000000 / kFps};
    pw_loop_update_timer(pw_main_loop_get_loop(d.loop), d.timer, &first, &every, false);
    pw_main_loop_run(d.loop);
    return 0;
}
