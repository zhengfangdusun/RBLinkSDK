/**
 * @file rbfclone.c
 * @brief Linux 文件/串口克隆参考例程：导出、显式恢复出厂、导入。
 *
 * 每条命令独立运行，完整流程见 README.md。
 * SDK 回调仅处理存储和状态通知，finish/cancel 由主线程调用。
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include "rbf_api.h"
#include "rbf_clone.h"

static int uart = -1, output = -1, input = -1;
static off_t input_size;
static int inventories;
static int trace_tx;
static struct termios saved;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int versions, ready, exited, errors;
static RBF_clone_status_t last;
static unsigned chunks, bytes;
static volatile sig_atomic_t stop;

static long long now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void signalled(int s)
{
    (void)s;
    stop = 1;
}

static int fullwrite(int fd, const void *data, int n)
{
    const unsigned char *p = data;
    int done = 0;

    while (done < n)
    {
        int r = write(fd, p + done, n - done);
        if (r < 0 && errno == EINTR)
        {
            continue;
        }
        if (r <= 0)
        {
            return -1;
        }
        done += r;
    }
    return done;
}

static int read_uart(unsigned char *buf, int n, int ms)
{
    struct pollfd p = {uart, POLLIN, 0};
    int r = poll(&p, 1, ms > 0 ? ms : 0);

    if (r <= 0)
    {
        return 0;
    }
    r = read(uart, buf, n);
    return r < 0 && (errno == EAGAIN || errno == EINTR) ? 0 : r;
}

static int write_uart(unsigned char *buf, int n)
{
    /* 仅在显式开启诊断时打印完整负载。 */
    if (n >= 8)
    {
        printf("TX code=%02x size=%d\n", buf[4], n);
    }
    if (trace_tx)
    {
        printf("TX HEX:");
        for (int i = 0; i < n; i++)
        {
            printf(" %02X", buf[i]);
        }
        puts("");
    }
    return fullwrite(uart, buf, n);
}

static int version(RBF_hub_sw_ver_t *v)
{
    pthread_mutex_lock(&lock);
    versions++;
    printf("HUB version=%u.%u.%u build=%02u%02u%02u%02u\n",
           v->major, v->sub, v->patch, v->build_year,
           v->build_month, v->build_date, v->build_no);
    pthread_mutex_unlock(&lock);
    return 0;
}

/* 导出回调：完整写盘并同步成功后，SDK 才确认这一片。 */
static int store(void *u, uint16_t total, uint16_t seq, const uint8_t *data, uint16_t size)
{
    (void)u;
    (void)total;
    (void)seq;
    if (fullwrite(output, data, size) != size || fsync(output))
    {
        return -1;
    }

    pthread_mutex_lock(&lock);
    chunks++;
    bytes += size;
    pthread_mutex_unlock(&lock);
    return 0;
}

/* 导入回调：按序号读取固定偏移，重试时返回相同字节。 */
static int load(void *u, uint16_t seq, uint8_t *data, uint16_t capacity, uint16_t *size)
{
    (void)u;
    off_t offset = (off_t)(seq - 1) * 234;
    if (!seq || offset >= input_size || capacity < 234)
    {
        return -1;
    }

    int n = input_size - offset > 234 ? 234 : input_size - offset;
    if (pread(input, data, n, offset) != n)
    {
        return -1;
    }
    *size = n;
    return 0;
}

static int inventory(RBF_dev_id_t *ids, int count)
{
    pthread_mutex_lock(&lock);
    printf("INVENTORY count=%d", count);
    for (int i = 0; i < count; i++)
    {
        printf(" %u:%u", ids[i].cat, ids[i].no);
    }
    puts("");
    inventories++;
    pthread_mutex_unlock(&lock);
    return 0;
}

static int query_inventory(void)
{
    int initial;
    pthread_mutex_lock(&lock);
    initial = inventories;
    pthread_mutex_unlock(&lock);
    if (rbf_get_register_info())
    {
        return -1;
    }

    long long end = now() + 6000;
    while (now() < end && !stop)
    {
        pthread_mutex_lock(&lock);
        int ok = inventories > initial;
        pthread_mutex_unlock(&lock);
        if (ok)
        {
            return 0;
        }
        usleep(10000);
    }
    return -1;
}

static void event(void *u, const RBF_clone_event_t *e)
{
    (void)u;
    pthread_mutex_lock(&lock);
    last = e->status;
    printf("EVENT type=%d state=%d chunks=%u/%u error=%d hub=%u exit=%d confirmed=%d\n",
           e->type, last.state, last.confirmed_chunks, last.total_chunks,
           last.error, last.hub_error, last.exit_reason, last.remote_exit_confirmed);
    if (e->type == RBF_CLONE_EVT_DATA_READY)
    {
        ready = 1;
    }
    if (e->type == RBF_CLONE_EVT_EXITED)
    {
        exited = 1;
    }
    if (e->type == RBF_CLONE_EVT_ERROR)
    {
        errors++;
    }
    pthread_mutex_unlock(&lock);
}

static int query_version(void)
{
    int initial, ok = 0;
    pthread_mutex_lock(&lock);
    initial = versions;
    pthread_mutex_unlock(&lock);
    if (rbf_get_hub_version())
    {
        return -1;
    }

    long long end = now() + 6000;
    while (now() < end && !stop)
    {
        pthread_mutex_lock(&lock);
        ok = versions > initial;
        pthread_mutex_unlock(&lock);
        if (ok)
        {
            return 0;
        }
        usleep(10000);
    }
    return -1;
}

int main(int argc, char **argv)
{
    int exporting = argc == 4 && !strcmp(argv[1], "export");
    int resetting = argc == 3 && !strcmp(argv[1], "factory-reset");
    int importing = argc == 4 && !strcmp(argv[1], "import");

    if (!exporting && !importing && !resetting && !(argc == 3 && !strcmp(argv[1], "probe")))
    {
        fprintf(stderr,
                "Usage: %s probe UART | export UART NEW_FILE | "
                "factory-reset UART | import UART RAW_FILE\n"
                "Export preserves devices on current firmware. "
                "factory-reset UART explicitly clears devices. "
                "Import replaces HUB data.\n", argv[0]);
        return 2;
    }
    trace_tx = getenv("RBF_CLONE_TRACE") && !strcmp(getenv("RBF_CLONE_TRACE"), "1");
    setvbuf(stdout, NULL, _IOLBF, 0);
    umask(077);
    signal(SIGINT, signalled);
    signal(SIGTERM, signalled);

    /* 准备备份文件；导出禁止覆盖已有文件。 */
    if (exporting)
    {
        output = open(argv[3], O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (output < 0)
        {
            perror("export file");
            return 2;
        }
    }
    if (importing)
    {
        struct stat st;
        input = open(argv[3], O_RDONLY);
        if (input < 0 || fstat(input, &st) || !S_ISREG(st.st_mode) ||
            st.st_size <= 0 || st.st_size > (off_t)65535 * 234)
        {
            fprintf(stderr, "Invalid input file\n");
            return 2;
        }
        input_size = st.st_size;
    }

    uart = open(argv[2], O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (uart < 0 || ioctl(uart, TIOCEXCL) < 0 || tcgetattr(uart, &saved))
    {
        perror("exclusive UART");
        return 2;
    }
    struct termios t = saved;
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~CRTSCTS;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(uart, TCSANOW, &t))
    {
        perror("termios");
        return 2;
    }
    fcntl(uart, F_SETFL, 0);

    RBF_port_t port = {read_uart, write_uart, NULL};
    RBF_evt_callbacks_t cb = {0};
    cb.rbf_hub_ver_handle = version;
    cb.rbf_dev_register_info_handle = inventory;
    int result = 1;
    if (rbf_set_port(&port) || rbf_register_evt_callback(&cb) || rbf_init())
    {
        goto end;
    }
    if (query_version())
    {
        puts("FAIL: initial real HUB version timeout");
        goto end;
    }
    if (!exporting && !importing && !resetting)
    {
        if (query_inventory())
        {
            goto end;
        }
        puts("PASS: real UART/HUB version query");
        result = 0;
        goto end;
    }

    RBF_clone_evt_callbacks_t clone = {NULL, store, load, event};
    RBF_clone_options_t opt = {5000, 15000, 2};
    if (rbf_clone_register_evt_callback(&clone) ||
        (resetting ? rbf_hub_factory_reset_start(1, &opt) :
         exporting ? rbf_clone_export_start(&opt) :
                     rbf_clone_import_start((input_size + 233) / 234, &opt)))
    {
        puts("FAIL: operation start rejected");
        goto end;
    }

    /* 等待异步状态，由主线程完成导入或请求取消。 */
    int cancelled = 0, complete = 0, got_exit = 0, error = 0, finishing = 0;
    long long endtime = now() + 90000, cancel_end = 0;
    while (1)
    {
        pthread_mutex_lock(&lock);
        complete = ready;
        got_exit = exited;
        error = errors;
        pthread_mutex_unlock(&lock);
        if (got_exit)
        {
            break;
        }
        if (importing && complete && !finishing && !error && !stop)
        {
            if (!rbf_clone_finish())
            {
                finishing = 1;
            }
        }
        if (!cancelled && (error || stop || now() >= endtime))
        {
            if (!rbf_clone_cancel())
            {
                cancelled = 1;
                cancel_end = now() + 20000;
                printf("CANCEL accepted: reason=%s sdk_error=%d; waiting remote confirmation\n",
                       error ? "SDK error" : stop ? "signal" : "deadline", last.error);
            }
            else if (now() > endtime + 20000)
            {
                puts("FAIL: cancel not accepted");
                break;
            }
        }
        if (cancelled && now() >= cancel_end)
        {
            puts("FAIL: cancel exit unconfirmed");
            break;
        }
        usleep(10000);
    }

    pthread_mutex_lock(&lock);
    int restored = got_exit && last.remote_exit_confirmed &&
                   last.exit_reason == RBF_CLONE_EXIT_COMPLETED;
    printf("TRANSFER ready=%d chunks=%u bytes=%u errors=%d completed=%d\n",
           ready, chunks, bytes, errors, restored);
    pthread_mutex_unlock(&lock);
    if (restored && (!exporting || !fsync(output)))
    {
        usleep(500000);
        if (!query_version() && !query_inventory() && !error)
        {
            puts("PASS: real HUB transfer complete + version/inventory after exit");
            result = 0;
        }
    }

end:
    tcsetattr(uart, TCSANOW, &saved);
    fflush(NULL);
    /* SDK 工作线程随进程退出，避免提前关闭共享 fd 引起竞态。 */
    _exit(result);
}
