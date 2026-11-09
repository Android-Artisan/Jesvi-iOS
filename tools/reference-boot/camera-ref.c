/* Bounded Samsung V4L2 front sensor stream for the temporary reference boot. */
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Samsung's videodev2_exynos_camera.h; sensor node s_ctrl starts CSIS + CIS. */
#define V4L2_CID_IS_S_STREAM (V4L2_CTRL_CLASS_CAMERA | 0x1000 | 14)
#define V4L2_CID_IS_G_STREAM (V4L2_CTRL_CLASS_CAMERA | 0x1000 | 15)
#define V4L2_CID_IS_S_SENSOR_SIZE (V4L2_CTRL_CLASS_CAMERA | 0x1000 | 69)

int main(void)
{
    const char *path = "/dev/video102"; /* EXYNOS_VIDEONODE_FIMC_IS=100 + SS1 video ID=2 */
    struct v4l2_capability cap = {0};
    struct v4l2_format fmt = {0};
    struct v4l2_control stream = {.id = V4L2_CID_IS_S_STREAM, .value = 1};
    struct v4l2_control state = {.id = V4L2_CID_IS_G_STREAM};
    struct v4l2_control size = {.id = V4L2_CID_IS_S_SENSOR_SIZE,
                                .value = (1824 << 16) | 1368};
    struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    int fd = open(path, O_RDWR | O_CLOEXEC);
    unsigned input = 1U << 16; /* INPUT_POSITION_SHIFT=16, front position=1 */
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return 1; }
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "QUERYCAP: %s\n", strerror(errno)); close(fd); return 2;
    }
    printf("node=%s driver=%s card=%s caps=%#x\n", path, cap.driver, cap.card, cap.capabilities);
    fflush(stdout);
    if (ioctl(fd, VIDIOC_S_INPUT, &input) < 0) {
        fprintf(stderr, "S_INPUT front=%#x: %s\n", input, strerror(errno)); close(fd); return 3;
    }
    puts("S_INPUT front accepted"); fflush(stdout);
    if (ioctl(fd, VIDIOC_S_CTRL, &size) < 0) {
        fprintf(stderr, "S_CTRL sensor size: %s\n", strerror(errno)); close(fd); return 4;
    }
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) {
        fprintf(stderr, "S_PARM 30 fps: %s\n", strerror(errno)); close(fd); return 5;
    }
    puts("sensor size and 30 fps accepted"); fflush(stdout);
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = 1824;
    fmt.fmt.pix_mp.height = 1368;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SBGGR10;
    fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "S_FMT 1824x1368 RAW10: %s\n", strerror(errno)); close(fd); return 6;
    }
    printf("S_FMT accepted %ux%u format=%#x\n", fmt.fmt.pix_mp.width,
           fmt.fmt.pix_mp.height, fmt.fmt.pix_mp.pixelformat);
    fflush(stdout);
    if (ioctl(fd, VIDIOC_S_CTRL, &stream) < 0) {
        fprintf(stderr, "S_CTRL IS_S_STREAM=1: %s\n", strerror(errno));
        close(fd); return 7;
    }
    puts("S_CTRL stream started"); fflush(stdout);
    for (int i = 0; i < 5; i++) {
        sleep(1);
        if (ioctl(fd, VIDIOC_G_CTRL, &state) == 0)
            printf("second=%d G_STREAM=%d\n", i + 1, state.value);
        else
            fprintf(stderr, "G_STREAM: %s\n", strerror(errno));
        fflush(stdout);
    }
    stream.value = 0;
    if (ioctl(fd, VIDIOC_S_CTRL, &stream) < 0)
        fprintf(stderr, "S_CTRL IS_S_STREAM=0: %s\n", strerror(errno));
    close(fd);
    return 0;
}
