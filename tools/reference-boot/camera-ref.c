/* Bounded Samsung V4L2 front sensor stream for the temporary reference boot. */
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Samsung's videodev2_exynos_camera.h; sensor node s_ctrl starts CSIS + CIS. */
#define V4L2_CID_IS_S_STREAM (V4L2_CTRL_CLASS_CAMERA | 0x1000 | 14)
#define V4L2_CID_IS_G_STREAM (V4L2_CTRL_CLASS_CAMERA | 0x1000 | 15)
#define V4L2_CID_IS_G_MIPI_ERR (V4L2_CTRL_CLASS_CAMERA | 0x1000 | 62)
#define V4L2_CID_IS_S_SENSOR_SIZE (V4L2_CTRL_CLASS_CAMERA | 0x1000 | 69)
#define ION_IOC_ALLOC _IOWR('I', 0, struct ion_allocation_data)
#define CAMERA_IMAGE_BYTES (6U * 1024U * 1024U)
#define CAMERA_META_BYTES (1U * 1024U * 1024U)
struct ion_allocation_data {
    uint64_t len;
    uint32_t heap_id_mask, flags, fd, unused;
};
#define CAMERA_CHECKPOINT_OFFSET 0x1e9d04000ULL
static int log_fd = -1;
static unsigned checkpoint_number;
static int pmic_fd = -1;
static uint8_t buck2_before;
static bool buck2_changed;
static int sensor_fd = -1;

static int sensor_read8(uint16_t reg, uint8_t *value)
{
    uint8_t address[] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    struct i2c_msg msg[] = {
        {.addr = 0x10, .len = sizeof(address), .buf = address},
        {.addr = 0x10, .flags = I2C_M_RD, .len = 1, .buf = value}
    };
    struct i2c_rdwr_ioctl_data req = {.msgs = msg, .nmsgs = 2};
    return sensor_fd >= 0 ? ioctl(sensor_fd, I2C_RDWR, &req) : -1;
}

static int pmic_read(uint8_t reg, uint8_t *value)
{
    struct i2c_msg msg[] = {
        {.addr = 0x59, .len = 1, .buf = &reg},
        {.addr = 0x59, .flags = I2C_M_RD, .len = 1, .buf = value}
    };
    struct i2c_rdwr_ioctl_data req = {.msgs = msg, .nmsgs = 2};
    return ioctl(pmic_fd, I2C_RDWR, &req);
}

static int pmic_write(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    struct i2c_msg msg = {.addr = 0x59, .len = sizeof(data), .buf = data};
    struct i2c_rdwr_ioctl_data req = {.msgs = &msg, .nmsgs = 1};
    return ioctl(pmic_fd, I2C_RDWR, &req);
}

static void restore_buck2(void)
{
    uint8_t current;
    if (pmic_fd < 0) return;
    if (buck2_changed && pmic_read(0x0d, &current) >= 0)
        (void)pmic_write(0x0d, (current & ~0xc0U) | (buck2_before & 0xc0U));
    close(pmic_fd);
}

static void checkpoint(const char *stage)
{
    char page[4096] = {0};
    off_t offset = CAMERA_CHECKPOINT_OFFSET + (off_t)(checkpoint_number++ % 32) * sizeof(page);
    int n = snprintf(page, sizeof(page), "camera-ref vision checkpoint %u: %s\n",
                     checkpoint_number, stage);
    if (n < 0 || n >= (int)sizeof(page) ||
        pwrite(log_fd, page, sizeof(page), offset) != sizeof(page) || fsync(log_fd) != 0) {
        fprintf(stderr, "checkpoint %s failed: %s\n", stage, strerror(errno));
        exit(90);
    }
    puts(page); fflush(stdout);
}

static void print_csi_interrupts(int second)
{
    FILE *interrupts = fopen("/proc/interrupts", "r");
    char line[512];
    if (!interrupts) {
        printf("second=%d CSI interrupts unavailable: %s\n", second, strerror(errno));
        return;
    }
    while (fgets(line, sizeof(line), interrupts)) {
        if (strstr(line, " CSI\n") || strstr(line, " CSI ")) {
            printf("second=%d CSI interrupts: %s", second, line);
            break;
        }
    }
    fclose(interrupts);
}

int main(int argc, char **argv)
{
    const char *path = "/dev/video102"; /* EXYNOS_VIDEONODE_FIMC_IS=100 + SS1 video ID=2 */
    struct v4l2_capability cap = {0};
    struct v4l2_format fmt = {0};
    struct v4l2_control stream = {.id = V4L2_CID_IS_S_STREAM, .value = 1};
    struct v4l2_control state = {.id = V4L2_CID_IS_G_STREAM};
    struct v4l2_control mipi_error = {.id = V4L2_CID_IS_G_MIPI_ERR};
    struct v4l2_control size = {.id = V4L2_CID_IS_S_SENSOR_SIZE,
                                .value = (1824 << 16) | 1368};
    struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    struct v4l2_requestbuffers request = {
        .count = 2, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_DMABUF
    };
    enum v4l2_buf_type queue_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    int ionfd = -1, buffer_fds[2][2] = {{-1, -1}, {-1, -1}};
    const bool queue = true; /* stand-alone Vision with receiver DMA buffers */
    uint8_t buck2_after = 0;
    struct stat log_stat;
    unsigned long long log_size = 0;
    int fd;
    unsigned input = (1U << 26) | (1U << 16); /* VISION stand-alone + front position */
    if (argc != 4 || (log_fd = open(argv[1], O_WRONLY | O_CLOEXEC)) < 0 ||
        fstat(log_fd, &log_stat) < 0 || !S_ISBLK(log_stat.st_mode) ||
        ioctl(log_fd, BLKGETSIZE64, &log_size) < 0 ||
        log_size < CAMERA_CHECKPOINT_OFFSET + 32 * 4096ULL) {
        fprintf(stderr, "validated super block device required for checkpoints\n");
        return 91;
    }
    checkpoint("helper start, before BUCK2 enable");
    pmic_fd = open(argv[2], O_RDWR | O_CLOEXEC);
    if (pmic_fd < 0 || atexit(restore_buck2) != 0 ||
        pmic_read(0x0d, &buck2_before) < 0) {
        fprintf(stderr, "BUCK2 snapshot failed: %s\n", strerror(errno));
        return 92;
    }
    buck2_changed = true;
    if (pmic_write(0x0d, buck2_before | 0xc0U) < 0 ||
        pmic_read(0x0d, &buck2_after) < 0 ||
        (buck2_after & 0xc0U) != 0xc0U) {
        fprintf(stderr, "BUCK2 enable/readback failed: %s\n", strerror(errno));
        return 92;
    }
    checkpoint("BUCK2 enabled and read back, before camera open");
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return 1; }
    checkpoint("camera open returned, before QUERYCAP");
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "QUERYCAP: %s\n", strerror(errno)); close(fd); return 2;
    }
    checkpoint("QUERYCAP returned, before S_INPUT");
    printf("node=%s driver=%s card=%s caps=%#x\n", path, cap.driver, cap.card, cap.capabilities);
    fflush(stdout);
    if (ioctl(fd, VIDIOC_S_INPUT, &input) < 0) {
        fprintf(stderr, "S_INPUT front=%#x: %s\n", input, strerror(errno)); close(fd); return 3;
    }
    checkpoint("S_INPUT returned, before SENSOR_SIZE");
    sensor_fd = open(argv[3], O_RDWR | O_CLOEXEC);
    printf("SAK3J1 I2C device=%s open=%s\n", argv[3], sensor_fd >= 0 ? "yes" : "no");
    puts("S_INPUT front accepted"); fflush(stdout);
    if (ioctl(fd, VIDIOC_S_CTRL, &size) < 0) {
        fprintf(stderr, "S_CTRL sensor size: %s\n", strerror(errno)); close(fd); return 4;
    }
    checkpoint("SENSOR_SIZE returned, before S_PARM");
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) {
        fprintf(stderr, "S_PARM 30 fps: %s\n", strerror(errno)); close(fd); return 5;
    }
    checkpoint("S_PARM returned, before S_FMT");
    puts("sensor size and 30 fps accepted"); fflush(stdout);
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = 1824;
    fmt.fmt.pix_mp.height = 1368;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SBGGR10;
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "S_FMT 1824x1368 RAW10: %s\n", strerror(errno)); close(fd); return 6;
    }
    checkpoint("S_FMT returned, before STREAM_ON");
    printf("S_FMT accepted %ux%u format=%#x\n", fmt.fmt.pix_mp.width,
           fmt.fmt.pix_mp.height, fmt.fmt.pix_mp.pixelformat);
    fflush(stdout);
    if (queue) {
    checkpoint("before REQBUFS");
    if (ioctl(fd, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
        fprintf(stderr, "REQBUFS: %s count=%u\n", strerror(errno), request.count);
        close(fd); return 8;
    }
    checkpoint("REQBUFS returned, before ION allocation");
    ionfd = open("/dev/ion", O_RDWR | O_CLOEXEC);
    if (ionfd < 0) { fprintf(stderr, "open /dev/ion: %s\n", strerror(errno)); close(fd); return 9; }
    for (unsigned i = 0; i < 2; i++) {
        struct ion_allocation_data image = {.len = CAMERA_IMAGE_BYTES, .heap_id_mask = 0xffffffffU};
        struct ion_allocation_data meta = {.len = CAMERA_META_BYTES, .heap_id_mask = 0xffffffffU};
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {{0}};
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            .memory = V4L2_MEMORY_DMABUF,
            .index = i,
            .length = 2,
            .m.planes = planes
        };
        if (ioctl(ionfd, ION_IOC_ALLOC, &image) < 0 ||
            ioctl(ionfd, ION_IOC_ALLOC, &meta) < 0) {
            fprintf(stderr, "ION alloc %u: %s\n", i, strerror(errno));
            close(fd); close(ionfd); return 11;
        }
        buffer_fds[i][0] = (int)image.fd;
        buffer_fds[i][1] = (int)meta.fd;
        planes[0].m.fd = buffer_fds[i][0]; planes[0].length = CAMERA_IMAGE_BYTES;
        planes[1].m.fd = buffer_fds[i][1]; planes[1].length = CAMERA_META_BYTES;
        checkpoint(i == 0 ? "first ION pair allocated, before QBUF" : "second ION pair allocated, before QBUF");
        if (ioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
            fprintf(stderr, "QBUF %u: %s\n", i, strerror(errno));
            close(fd); close(ionfd); return 12;
        }
        checkpoint(i == 0 ? "first DMA buffer queued" : "second DMA buffer queued");
    }
    checkpoint("before queue STREAMON");
    if (ioctl(fd, VIDIOC_STREAMON, &queue_type) < 0) {
        fprintf(stderr, "queue STREAMON: %s\n", strerror(errno));
        close(fd); return 10;
    }
    checkpoint("queue STREAMON returned, before sensor STREAM_ON");
    }
    if (ioctl(fd, VIDIOC_S_CTRL, &stream) < 0) {
        fprintf(stderr, "S_CTRL IS_S_STREAM=1: %s\n", strerror(errno));
        close(fd); return 7;
    }
    checkpoint("STREAM_ON returned");
    puts("S_CTRL stream started"); fflush(stdout);
    print_csi_interrupts(0);
    for (int i = 0; i < 5; i++) {
        uint8_t sensor_count = 0;
        sleep(1);
        if (ioctl(fd, VIDIOC_G_CTRL, &state) == 0)
            printf("second=%d G_STREAM=%d\n", i + 1, state.value);
        else
            fprintf(stderr, "G_STREAM: %s\n", strerror(errno));
        if (ioctl(fd, VIDIOC_G_CTRL, &mipi_error) == 0)
            printf("second=%d G_MIPI_ERR=%d\n", i + 1, mipi_error.value);
        else
            fprintf(stderr, "G_MIPI_ERR: %s\n", strerror(errno));
        print_csi_interrupts(i + 1);
        if (sensor_read8(0x0005, &sensor_count) >= 0)
            printf("second=%d SAK3J1_FRAME_COUNT=%u\n", i + 1, sensor_count);
        else
            printf("second=%d SAK3J1_FRAME_COUNT=unavailable\n", i + 1);
        fflush(stdout);
    }
    stream.value = 0;
    checkpoint("before STREAM_OFF");
    if (ioctl(fd, VIDIOC_S_CTRL, &stream) < 0)
        fprintf(stderr, "S_CTRL IS_S_STREAM=0: %s\n", strerror(errno));
    if (queue && ioctl(fd, VIDIOC_STREAMOFF, &queue_type) < 0)
        fprintf(stderr, "queue STREAMOFF: %s\n", strerror(errno));
    close(fd);
    for (unsigned i = 0; i < 2; i++)
        for (unsigned j = 0; j < 2; j++)
            if (buffer_fds[i][j] >= 0) close(buffer_fds[i][j]);
    if (ionfd >= 0) close(ionfd);
    if (sensor_fd >= 0) close(sensor_fd);
    checkpoint("STREAM_OFF returned, camera closed");
    close(log_fd);
    return 0;
}
