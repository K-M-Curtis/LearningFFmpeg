// 简化版的ffplay，去掉非必要的操作和处理，去掉了AVFILTER的内容，去掉decoder的选择，不给设置解码器进行解码，
// 去掉循环切换流的操作。保留基本的播放器的功能，理解熟悉和简单解读简化版ffplay的基本功能
// 主要复杂的地方还是在音视频同步这块，很多地方都涉及到时钟的更新，时钟的更新主要是用来进行同步处理的，需要留意下
// 后面将基于简化版的ffplay，实现自己的播放器和播放接口
/* ====== Learning ffplay.c -- 简化版的ffplay ====== */
// https://ffmpeg.org/doxygen/5.1/ffplay_8c_source.html -- 代码参看链接，基于5.1版本

/*-----------------------------------
例子：
https://github.com/rambodrahmani/ffmpeg-video-player/blob/master/tutorial07/tutorial07.c#L1227
ffplay源码解析：
https://blog.csdn.net/weixin_41910694/article/details/115795452  -- 已看
https://juejin.cn/post/7054718370289188872  -- ffplay源码分析，-- 已看
https://github.com/leo4048111/ffplay-explained  -- ffplay源码分析
https://ffmpeg.xianwaizhiyin.net/ffplay/serial.html  -- ffplay源码分析
https://blog.csdn.net/weixin_39413066/article/details/122000565  --  已看
https://www.cnblogs.com/juju-go/p/16489044.html
-----------------------------------*/

/* ============= 阅记 ============= */
/*
在 FFplay 源码中有很多bool类型的变量，为什么不使用bool类型而使用int类型，这有几个原因：
1. 兼容性考虑
C 语言中没有原生的布尔类型，布尔类型是在 C99 标准中才引入的，而很多旧代码库在 C99 之前编写或需要在不支持 C99 的编译器上编译。为了保证代码的广泛兼容性，使用整型作为布尔值是一种通用的做法。
2. 内存对齐和性能
在某些架构和编译器下，布尔类型可能会占用一个字节的内存空间，但在实际使用中，特别是数组或结构体中，内存对齐可能导致比预期更多的内存占用。使用 int 类型可以避免这些潜在的内存对齐问题，在某些情况下还可以提升性能。
3. 代码一致性
FFmpeg 项目中的大部分代码都使用整型表示布尔值。为了保持代码风格的一致性和可读性，在 FFplay 中也采用了类似的做法。这使得代码的维护和理解更加统一和简洁。
4. 明确的语义
使用整型可以更清楚地表达变量的语义。例如，一个变量如果取值 0 和非 0 值，用整型可以更加明确地表示“真”和“假”的意义。此外，整型变量还可以用于位运算，提供更多的灵活性。
5. 历史原因
FFplay 及 FFmpeg 项目历史悠久，很多代码是在布尔类型被广泛接受之前编写的。为了避免大规模重构带来的风险和不兼容性问题，继续使用整型作为布尔值是一种稳妥的选择。
总的来说，FFplay 源码中使用整型而不是布尔类型，主要是出于兼容性、内存对齐、代码一致性、明确语义以及历史原因等方面的考虑
*/

/* ============= 关于 serial 的说法 ============= */
/*
https://blog.csdn.net/m0_60259116/article/details/126468915?spm=1001.2014.3001.5506
https://ffmpeg.xianwaizhiyin.net/ffplay/serial.html
**区别前后数据包是否连续，主要应⽤于seek操作**
serial的概念，主要用来区分是否连续数据，每做一次seek，该serial都会做+1的递增，以区分不同的播放序列。
序列号主要是给快进快退这个功能准备的。如果不能快进快退，那其实就不需要序列号。
clock的serial最终是来源于Frame的serial（取自Decoder的pkt_serial（接收自MyAVPacketList））
总的来说，所有serial的值都来源于PacketQueue的serial，当文件正常播放时，各个结构体中的serial值相同；但是当发生文件跳转事件，PacketQueue的serial值率先发生变化，
同时清空原有来保持的AVPacket数据，然后各个结构体之间发现自身的serial与原先PacketQueue的serial不同，于是进行相关的操作，最终实现文件跳转播放的功能。
*/

/* ============= 关于 framedrop 的说法 ============= */
/*
framedrop作用是控制帧丢弃策略。
在媒体播放过程中,如果播放设备的计算能力无法及时解码和渲染所有帧,就会导致帧率下降和画面卡顿。为了缓解这种情况,FFplay提供了 framedrop 选项来实现帧丢弃。
framedrop 的作用包括:
帧丢弃策略：
framedrop=0: 禁用帧丢弃,即所有帧都会被渲染,但可能导致卡顿。
framedrop=-1: 启用自动帧丢弃,FFplay 会根据当前的解码状况自动丢弃部分帧以保证播放流畅。
framedrop=1: 启用更积极的帧丢弃策略,FFplay 会更频繁地丢弃帧以维持较高的帧率。
帧丢弃机制：
FFplay 会优先丢弃非关键帧(B帧和P帧)来保证关键帧(I帧)的完整性。
当出现解码滞后时,FFplay 会根据当前的缓冲区填充情况自动调整丢弃策略,尽量保证播放的流畅性。
应用场景：
在性能有限的设备上播放高码率视频时,开启 framedrop 选项可以避免卡顿。
在网络条件较差时播放流媒体,开启 framedrop 可以减少因网络延迟造成的帧丢失。
在录制视频时,关闭 framedrop 可以确保所有帧都被记录下来,即使会导致短暂的卡顿。
*/

/* ============= 关于 seek_by_bytes 的说法 ============= */
/*
seek_by_bytes 函数在 ffplay 中用于通过字节偏移量进行视频文件的快进/快退操作,与基于时间的seek操作有一些区别:
基于字节的seek:
seek_by_bytes 函数使用文件的字节偏移量作为参考,来定位视频的播放位置。这种方式适用于那些没有时间戳信息的视频文件,或者时间戳信息不准确的情况。
提高定位精度:
与基于时间的seek相比,基于字节的seek通常能提供更精确的定位。这对于需要精确定位的场景非常有用,如编辑视频时定位特定的画面。
不受时间戳影响:
基于字节的seek不受视频文件中时间戳的影响。即使时间戳信息有误或不完整,也能够准确定位到指定的字节位置。
适用于不同编码格式:
seek_by_bytes 函数可以处理各种视频编码格式,只要能够读取文件的字节偏移量即可。这使得它适用范围更广。
不保证同步:
需要注意的是,单纯通过字节偏移量进行seek,无法保证音频和视频的完全同步。因此在某些场景下,可能需要结合时间戳信息来进行更精确的seek操作。
总之, seek_by_bytes 函数提供了一种基于字节偏移量的快进/快退方式,相比基于时间戳的seek操作,它能在某些特殊情况下提供更精确的定位。但需要注意可能出现的同步问题。在实际使用中,通常会结合两种seek方式来满足不同的需求。
*/

/*
变速变调可分为：变速不变调和变调不变速。
语音变速不变调是指保持音调和语义保持不变，语速变快或变慢。该过程表现为语谱图在时间轴上如手风琴般压缩或者扩展。那也就是说，基频值几乎不变，对应于音调不变；
整个时间过程被压缩或者扩展，声门周期的数目减小或者增加，即声道运动速率发生改变，语速也随之变化。对应于语音产生模型，激励和系统经历与原始发音情况几乎相同的状态，但持续时间相比原来或长或短。
严格地讲，基频和音调是两个不同的概念，基频是指声带振动的频率，音调是指人类对基频的主观感知，但是两者变化基本一致，即基频越高，音调越高，基频越低，音调越低，音调是由基频决定的。
因此，语音变调不变速就是指改变说话人基频的大小，同时保持语速和语义不变，即保持短时频谱包络(共振峰的位置和带宽)和时间过程基本不变。对应于语音产生模型，变调改变了激励源；声道模型的共振峰参数几乎不变，保证了语义和语速不变。
综上所述，变速改变声道运动速率，力求保持激励源不变；变调改变激励源，力求保持声道的共振峰信息不变。但是声源和声道不是相互独立的，在改变声源时，必然也会非线性的影响声道，同样地，改变声道时也会或多或少的影响声源，两者之间相互影响，相互作用。
*/

/* ============= 音视频解码器有很多种类 ============= */
/* 主要包括以下几类，了解一下就行：
视频解码器：
H.264/AVC
H.265/HEVC
VP9
VP8
MPEG-2
MPEG-4 Part 2
AV1
Theora
DivX
XviD

音频解码器：
MP3
AAC
Vorbis
Opus
PCM
FLAC
WMA
AC-3 (Dolby Digital)
DTS
ALAC (Apple Lossless)

特殊用途解码器：
DNxHD
ProRes
Cinepak
Indeo
Sorenson Video
Huffyuv
Lagarith
*/

/* ========= Some function descriptions =========
FFMAX
功能: 返回两个参数中的最大值。
定义:
#define FFMAX(a,b) ((a) > (b) ? (a) : (b))

FFMIN
功能: 返回两个参数中的最小值。
定义:
#define FFMIN(a,b) ((a) > (b) ? (b) : (a))

avformat_seek_file() 和 av_seek_frame() 函数在 FFmpeg 中的作用和使用场景有以下几点不同:
功能范围:
avformat_seek_file() 是一个较高层次的函数,可以对整个输入文件进行seek操作,支持多种seek模式。
av_seek_frame() 则更低层次,只能对单个流(视频、音频等)进行seek操作。
seek模式:
avformat_seek_file() 支持多种seek模式,如 AVSEEK_FLAG_BYTE(基于字节)、AVSEEK_FLAG_FRAME(基于帧)、AVSEEK_FLAG_ANY(任意位置) 等。
av_seek_frame() 只支持基于时间戳的seek模式。
返回值:
avformat_seek_file() 返回的是seek操作的结果状态码,可能是成功、失败或部分成功等。
av_seek_frame() 返回的是实际seek到的时间戳位置。
使用场景:
avformat_seek_file() 更适用于需要对整个输入文件进行复杂seek操作的场景,如视频编辑、文件导航等。
av_seek_frame() 则更适用于对单个视频/音频流进行简单的seek操作,如视频播放器中的快进/快退功能。

av_clip(int a, int min, int max)
功能：
将一个值限制在一个指定的范围
如果 a 的值小于 min，则返回 min。
如果 a 的值大于 max，则返回 max。
如果 a 的值在 min 和 max 之间，则直接返回 a。

SDL_RenderCopyEx() 和 SDL_RenderCopy() 都是 SDL 库中用于渲染纹理(texture)的函数,但它们有一些区别:
功能差异:
SDL_RenderCopy() 用于简单地将一个纹理渲染到目标渲染器上,不支持旋转、翻转等操作。
SDL_RenderCopyEx() 除了基本的渲染操作外,还支持对纹理进行旋转、翻转等操作。
========= Some function descriptions =========*/

#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/fifo.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>

const char program_name[] = "SimpleMediaPlayer";

/**
 * Debug flag.
 */
#define DEBUG 1

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB 20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000  // us，1秒

// 这个值设置不宜过大，不然解码后帧数据占用内容会很大
#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE   \
  FFMAX(SAMPLE_QUEUE_SIZE, \
        FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

typedef struct MyAVPacketList {
  AVPacket *pkt;
  int serial;
} MyAVPacketList;

typedef struct PacketQueue {
  AVFifo *pkt_list;
  int nb_packets;
  int size;
  int64_t duration;
  int abort_request;
  int serial;
  SDL_mutex *mutex;
  SDL_cond *cond;
} PacketQueue;

typedef struct AudioParams {
  int freq;                   // 采样率
  AVChannelLayout ch_layout;  // 通道布局，比如2.1声道，5.1声道等
  // 音频采样格式，比如AV_SAMPLE_FMT_S16表示为有符号16bit深度，交错排列模式。
  enum AVSampleFormat fmt;
  int frame_size;  // 一个采样单元占用的字节数（比如2通道时，则左右通道各采样一次合成一个采样单元）
  int bytes_per_sec;  // 一秒时间的字节数，比如采样率48Khz，2 channel，16bit，则一秒48000*2*16/8=192000
} AudioParams;

/* 用于管理视频和音频流的播放时钟 */
typedef struct Clock {
  double pts;  // clock base, 当前时钟时间
  // clock base minus time at which we updated the clock, 时钟漂移量，当前pts与当前系统时钟的差值
  double pts_drift;
  double last_updated;  // 上次更新时间
  // 时钟速度, 默认为 1.0 表示正常速度,大于 1.0 为快速播放,小于 1.0 为慢速播放
  double speed;
  // 作用: serial 是每次从FrameQueue 里面拿frame的时候，就会把 serial 赋值为 frame 的序列号,
  int serial;  // clock is based on a packet with this serial, 时钟基于具有此序列的数据包, 时钟序列号, 用于标识不同的时钟实例, 用于判断播放是否连续的标志，用于seek
  int paused;  // 是否暂停
  // 是一直指向 PacketQueue 队列的序列号，seek的时候，PacketQueue 队列的序列号会 +1 ，这时候 Clock 的 serial 就会不等于 queue_serial
  // pointer to the current packet queue serial, used for obsolete clock detection, 指向当前数据包队列序列的指针，用于过时时钟检测, 关联的队列序列号, 用于时钟同步, 确保时钟与缓存数据保持一致
  int *queue_serial;
} Clock;

/* 帧数据，为了⾳频、视频、字幕帧通⽤，部分成员只对特定类型 */
/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
  AVFrame *frame;   // 解码后的音频或视频帧数据
  AVSubtitle sub;   // 解码后的字幕数据
  int serial;       // 用于标识帧的顺序，seek时使用
  double pts;       // presentation timestamp for the frame
  double duration;  // estimated duration of the frame
  int64_t pos;      // byte position of the frame in the input file
  int width;
  int height;
  int format;
  // 帧的宽高比（16:9，4:3…），该值来⾃AVFrame结构体的sample_aspect_ratio变量
  AVRational sar;
  int uploaded;  // 标志帧是否已上传到渲染缓冲区，即当前帧是否已显示
  int flip_v;  // 帧是否需要垂直翻转
} Frame;

/* ========= FrameQueue =========
 帧队列，用数组实现的一个FIFO，一端写一端读
 引入了读取节点但节点不出队列的操作、读取下一节点也不出队列等等的操作
 缓存从解码器获取的视频/音频帧数据,以便于后续的播放。这样可以减少解码器和播放器之间的同步压力
 多线程环境，解码线程和播放线程可以并发地操作帧队列，提高整体的执行效率
*/
typedef struct FrameQueue {
  Frame queue[FRAME_QUEUE_SIZE];
  int rindex;  // 读索引。待播放时读取此帧进行播放，播放后此帧成为上一帧
  int windex;  // 写索引
  int size;    // 当前总帧数
  int max_size;   // 可存储最大帧数
  int keep_last;  // = 1说明要在队列里面保持最后一帧的数据不释放，只在销毁队列的时候才将其真正释放
  int rindex_shown;  // 初始化为0，配合keep_last=1使用
  SDL_mutex *mutex;
  SDL_cond *cond;
  PacketQueue *pktq;
} FrameQueue;

enum {
  AV_SYNC_AUDIO_MASTER, /* default choice */
  AV_SYNC_VIDEO_MASTER,
  AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

/* ========= 解码器相关 ========= */
typedef struct Decoder {
  AVPacket *pkt;
  PacketQueue *queue;
  AVCodecContext *avctx;
  int pkt_serial;  // 包序列，用于seek
  int finished;    // 解码器是否处于工作状态；=0处于工作状态
  int packet_pending;  // 是否有待处理的数据包,避免丢失数据
  // 检查到packet队列空时发送 signal缓存read_thread读取数据
  SDL_cond *empty_queue_cond;
  int64_t start_pts;        // stream的start time
  AVRational start_pts_tb;  // stream的time_base
  int64_t next_pts;         // 最近一次解码后的frame的pts
  AVRational next_pts_tb;
  SDL_Thread *decoder_tid;
} Decoder;

typedef struct VideoState {
  SDL_Thread *read_tid;
  const AVInputFormat *iformat;
  int abort_request;  // 视频是否退出或中断播放
  int force_refresh;  // 立即强制刷新
  int paused;
  int last_paused;            // 暂存暂停/播放状态
                              /* 
视频流中的附件图像数据通常指的是以下几种类型:
封面图像(Cover Art)：许多视频或音频文件都会包含一个封面图像,用于在播放器界面中显示。这种图像通常是专辑封面、电影海报等。
缩略图(Thumbnails)：一些视频文件会包含代表视频内容的缩略图,用于在播放器界面中快速预览视频内容。
字幕图片：一些字幕格式(如 ASS/SSA)允许在字幕中嵌入图片,用于显示特殊字符或插图。
字体文件：一些视频文件会包含自定义字体文件,用于渲染字幕或特殊字符。
其他附件图像：除了上述常见的类型,视频文件还可能包含各种其他类型的附件图像,例如作为视频内容一部分的插图、背景图等。
*/
  int queue_attachments_req;  //视频流的附件数据，这个不要了
  int seek_req;
  int seek_flags;
  int64_t seek_pos;       // 当前位置+增量
  int64_t seek_rel;       // 增量
  int read_pause_return;  // 用于流媒体，网络流(e.g. RTSP stream)
  AVFormatContext *ic;
  int realtime;  //是否为实时流

  Clock audclk;
  Clock vidclk;
  Clock extclk;

  FrameQueue pictq;  //视频帧队列
  FrameQueue subpq;  //字幕队列
  FrameQueue sampq;  //采样队列

  Decoder auddec;  //音频解码器
  Decoder viddec;  //视频解码器
  Decoder subdec;  //字幕解码器

  int audio_stream;

  int av_sync_type;

  double audio_clock;      // 当前音频帧pts+当前帧duration
  int audio_clock_serial;  // 播放序列，用于seek

  double audio_diff_cum; /* used for AV difference average computation */
  double audio_diff_avg_coef;
  double audio_diff_threshold;
  int audio_diff_avg_count;

  AVStream *audio_st;
  PacketQueue audioq;
  int audio_hw_buf_size;
  uint8_t *audio_buf;
  uint8_t *audio_buf1;
  unsigned int audio_buf_size; /* in bytes */
  unsigned int audio_buf1_size;
  int audio_buf_index; /* in bytes */
  int audio_write_buf_size;
  int audio_volume;  // 音量
  int muted;         // 是否静音
  struct AudioParams audio_src;
  struct AudioParams audio_tgt;
  struct SwrContext *swr_ctx;
  int frame_drops_early;  // 丢弃视频packet计数
  int frame_drops_late;   // 丢弃视频frame计数

  enum ShowMode {
    SHOW_MODE_NONE = -1,  // 无显示
    SHOW_MODE_VIDEO = 0,  // 显示视频
    // SHOW_MODE_WAVES,   // 显示声波
    // SHOW_MODE_RDFT,      // 自适应滤波器
    SHOW_MODE_NB
  } show_mode;
  int16_t sample_array[SAMPLE_ARRAY_SIZE];  // 采样数组
  int sample_array_index;                   // 采样索引

  SDL_Texture *sub_texture;  // 字幕
  SDL_Texture *vid_texture;  // 视频

  // 字幕
  int subtitle_stream;
  AVStream *subtitle_st;
  PacketQueue subtitleq;

  double frame_timer;  // 最后一帧播放的时间
  // double frame_last_returned_time;  // 上一个返回时间
  // double frame_last_filter_delay;   // 上一个过滤器延时

  // 视频
  int video_stream;
  AVStream *video_st;
  PacketQueue videoq;

  // 帧间最大间隔
  // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
  double max_frame_duration;
  struct SwsContext *sub_convert_ctx;
  int eof;  // 是否读取结束

  char *filename;
  int width, height, xleft, ytop;
  int step;  //是否逐帧播放

  // 与循环切换流相关参数，此简化版不支持循环切换流的操作，可去掉处理
  int last_video_stream, last_audio_stream, last_subtitle_stream;

  SDL_cond *continue_read_thread;
} VideoState;

/* options specified by the user */
static const AVInputFormat *file_iformat;
static const char *input_filename;  // 播放的文件名
static const char *window_title;  //不强制设置，从文件和媒体数据中获取
static int default_width = 640;
static int default_height = 480;
static int screen_width = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;
static int audio_disable = 0;
static int video_disable = 0;
static int subtitle_disable = 0;
static const char *wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static int seek_by_bytes = -1;
static int display_disable = 0;  // 是否显示视频内容
static int borderless = 0;
static int alwaysontop = 0;
static int startup_volume = 100;
/* 
show_status主要作用是在播放过程中显示媒体的状态信息。
具体来说,当开启 show_status 选项时,FFplay 会在播放过程中持续输出以下信息:
当前时间戳，视频/音频帧数，帧速率，码率，缓冲区状态，解码器信息，其他一些统计数据
*/
static int show_status = -1;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
// 控制视频解码输出的分辨率，降低视频分辨率，节省处理资源，快速预览，这个不用设置，就让他等于0
static int lowres = 0;
static int autoexit = 0;  // exit at the end视频结束后是否自动退出
static int exit_on_keydown = 0;
static int exit_on_mousedown = 0;
static int loop = 1;
static int framedrop = -1;  //不用设置，让他自己判断
static int infinite_buffer = -1;  // =1的话不限制输入缓冲区大小（对实时流有用）
static enum ShowMode show_mode = SHOW_MODE_VIDEO;
static int64_t cursor_last_shown;
static int cursor_hidden = 0;
static int is_full_screen = 0;
static int64_t audio_callback_time;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_RendererInfo renderer_info = {0};
static SDL_AudioDeviceID audio_dev;

static const struct TextureFormatEntry {
  enum AVPixelFormat format;
  int texture_fmt;
} sdl_texture_format_map[] = {
    {AV_PIX_FMT_RGB8, SDL_PIXELFORMAT_RGB332},
    {AV_PIX_FMT_RGB444, SDL_PIXELFORMAT_RGB444},
    {AV_PIX_FMT_RGB555, SDL_PIXELFORMAT_RGB555},
    {AV_PIX_FMT_BGR555, SDL_PIXELFORMAT_BGR555},
    {AV_PIX_FMT_RGB565, SDL_PIXELFORMAT_RGB565},
    {AV_PIX_FMT_BGR565, SDL_PIXELFORMAT_BGR565},
    {AV_PIX_FMT_RGB24, SDL_PIXELFORMAT_RGB24},
    {AV_PIX_FMT_BGR24, SDL_PIXELFORMAT_BGR24},
    {AV_PIX_FMT_0RGB32, SDL_PIXELFORMAT_RGB888},
    {AV_PIX_FMT_0BGR32, SDL_PIXELFORMAT_BGR888},
    {AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888},
    {AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888},
    {AV_PIX_FMT_RGB32, SDL_PIXELFORMAT_ARGB8888},
    {AV_PIX_FMT_RGB32_1, SDL_PIXELFORMAT_RGBA8888},
    {AV_PIX_FMT_BGR32, SDL_PIXELFORMAT_ABGR8888},
    {AV_PIX_FMT_BGR32_1, SDL_PIXELFORMAT_BGRA8888},
    {AV_PIX_FMT_YUV420P, SDL_PIXELFORMAT_IYUV},
    {AV_PIX_FMT_YUYV422, SDL_PIXELFORMAT_YUY2},
    {AV_PIX_FMT_UYVY422, SDL_PIXELFORMAT_UYVY},
    {AV_PIX_FMT_NONE, SDL_PIXELFORMAT_UNKNOWN},
};

// 比较两个音频格式是否兼容
// 对于单声道音频,平面格式和非平面格式是可以相互转换的
// static inline int cmp_audio_fmts(enum AVSampleFormat fmt1,
//                                  int64_t channel_count1,
//                                  enum AVSampleFormat fmt2,
//                                  int64_t channel_count2) {
//   /* If channel count == 1, planar and non-planar formats are the same */
//   if (channel_count1 == 1 && channel_count2 == 1)
//     return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
//   else
//     return channel_count1 != channel_count2 || fmt1 != fmt2;
// }

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt) {
  MyAVPacketList pkt1;
  int ret;

  if (q->abort_request) return -1;

  pkt1.pkt = pkt;
  pkt1.serial = q->serial;
  /*
   为什么使用 av_fifo_write？
   顺序处理：av_fifo_write 确保数据包按它们到达的顺序被存储和处理。这对于视频和音频流的同步至关重要。如果数据包处理顺序出现问题，可能会导致播放时的音视频不同步。
   线程安全：av_fifo_write 与 av_fifo_read 配合使用，提供了一个线程安全的方法来处理数据包队列。这样可以确保一个线程在写入数据包时，另一个线程可以安全地读取数据包，而不会出现竞态条件或数据损坏。
   动态增长：在初始化 FIFO 队列时使用了 AV_FIFO_FLAG_AUTO_GROW 标志，这使得队列在需要时可以动态增长，避免了队列溢出的问题。
  */
  ret = av_fifo_write(q->pkt_list, &pkt1, 1);
  if (ret < 0) return ret;
  q->nb_packets++;
  q->size += pkt1.pkt->size + sizeof(pkt1);
  q->duration += pkt1.pkt->duration;
  /* XXX: should duplicate packet data in DV case */
  SDL_CondSignal(q->cond);
  return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
  AVPacket *pkt1;
  int ret;

  pkt1 = av_packet_alloc();
  if (!pkt1) {
    av_packet_unref(pkt);
    return -1;
  }
  av_packet_move_ref(pkt1, pkt);

  SDL_LockMutex(q->mutex);
  ret = packet_queue_put_private(q, pkt1);
  SDL_UnlockMutex(q->mutex);

  if (ret < 0) av_packet_free(&pkt1);

  return ret;
}

// 放⼊空包意味着流的结束，⼀般在媒体数据读取完成的时候放⼊空包。放⼊空包，⽬的是为了冲刷解码器，将编码器⾥⾯所有frame都读取出来
static int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt,
                                       int stream_index) {
  pkt->stream_index = stream_index;
  return packet_queue_put(q, pkt);
}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q) {
  memset(q, 0, sizeof(PacketQueue));
  q->pkt_list =
      av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
  if (!q->pkt_list) return AVERROR(ENOMEM);
  q->mutex = SDL_CreateMutex();
  if (!q->mutex) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  q->cond = SDL_CreateCond();
  if (!q->cond) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  q->abort_request = 1;
  return 0;
}

// ⽤于将packet队列中的所有节点清除，包括节点对应的AVPacket。⽐如⽤于退出播放和seek播放
static void packet_queue_flush(PacketQueue *q) {
  MyAVPacketList pkt1;

  SDL_LockMutex(q->mutex);
  while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
    av_packet_free(&pkt1.pkt);
  }
  q->nb_packets = 0;
  q->size = 0;  // 指向序列号的指针，用于存储获取到的数据包的序列号。
  q->duration = 0;
  q->serial++;
  SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q) {
  packet_queue_flush(q);
  av_fifo_freep2(&q->pkt_list);
  SDL_DestroyMutex(q->mutex);
  SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue *q) {
  SDL_LockMutex(q->mutex);

  q->abort_request = 1;

  SDL_CondSignal(q->cond);

  SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue *q) {
  SDL_LockMutex(q->mutex);
  q->abort_request = 0;
  q->serial++;
  SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block,
                            int *serial) {
  MyAVPacketList pkt1;
  int ret;

  SDL_LockMutex(q->mutex);

  for (;;) {
    if (q->abort_request) {
      ret = -1;
      break;
    }

    if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
      q->nb_packets--;
      q->size -= pkt1.pkt->size + sizeof(pkt1);
      q->duration -= pkt1.pkt->duration;
      av_packet_move_ref(pkt, pkt1.pkt);
      if (serial) {
        *serial = pkt1.serial;
      }
      av_packet_free(&pkt1.pkt);
      ret = 1;
      break;
    } else if (!block) {
      ret = 0;
      break;
    } else {
      SDL_CondWait(q->cond, q->mutex);
    }
  }
  SDL_UnlockMutex(q->mutex);
  return ret;
}

static int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue,
                        SDL_cond *empty_queue_cond) {
  memset(d, 0, sizeof(Decoder));
  d->pkt = av_packet_alloc();
  if (!d->pkt) return AVERROR(ENOMEM);
  d->avctx = avctx;
  d->queue = queue;
  d->empty_queue_cond = empty_queue_cond;
  d->start_pts = AV_NOPTS_VALUE;
  d->pkt_serial = -1;
  return 0;
}

// 从解码队列中获取数据包，并将其解码为视频或音频帧。
static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
  // 表示需要更多的数据才能进行解码，会去到packet_queue_get()
  int ret = AVERROR(EAGAIN);

  for (;;) {
    if (d->queue->serial == d->pkt_serial) {
      // 解码操作，不断尝试从解码器中获取帧数据
      do {
        if (d->queue->abort_request) {
          return -1;
        }

        switch (d->avctx->codec_type) {
          case AVMEDIA_TYPE_VIDEO:
            ret = avcodec_receive_frame(d->avctx, frame);
            if (ret >= 0) {
              frame->pts = frame->best_effort_timestamp;
            }
            break;
          case AVMEDIA_TYPE_AUDIO:
            ret = avcodec_receive_frame(d->avctx, frame);
            if (ret >= 0) {
              AVRational tb = (AVRational){1, frame->sample_rate};
              if (frame->pts != AV_NOPTS_VALUE)
                frame->pts =
                    av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
              else if (d->next_pts != AV_NOPTS_VALUE)
                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
              if (frame->pts != AV_NOPTS_VALUE) {
                d->next_pts = frame->pts + frame->nb_samples;
                d->next_pts_tb = tb;
              }
            }
            break;
        }
        // 解码完所有数据,需要刷新缓冲区并返回
        if (ret == AVERROR_EOF) {
          d->finished = d->pkt_serial;
          avcodec_flush_buffers(d->avctx);
          return 0;
        }
        // 解码成功
        if (ret >= 0) {
          return 1;
        }
      } while (ret != AVERROR(EAGAIN));
    }

    // 获取数据包
    do {
      if (d->queue->nb_packets == 0) {
        SDL_CondSignal(d->empty_queue_cond);
      }
      // 是否有待处理的数据包
      if (d->packet_pending) {
        d->packet_pending = 0;
      } else {
        int old_serial = d->pkt_serial;
        if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0)
          return -1;
        if (old_serial != d->pkt_serial) {
          avcodec_flush_buffers(d->avctx);
          d->finished = 0;
          d->next_pts = d->start_pts;
          d->next_pts_tb = d->start_pts_tb;
        }
      }
      if (d->queue->serial == d->pkt_serial) {
        break;
      }
      av_packet_unref(d->pkt);
    } while (1);

    // 数据包处理，解码数据包
    if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
      int got_frame = 0;
      ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, d->pkt);
      if (ret < 0) {
        ret = AVERROR(EAGAIN);
      } else {
        // 如果got_frame大于 0 但 d->pkt->data 为空,则说明字幕帧还没有完全解码出来。此时会设置 d->packet_pending 标志位为 1,表示还有数据包需要继续处理。
        if (got_frame && !d->pkt->data) {
          d->packet_pending = 1;
        }
        ret = got_frame ? 0 : (d->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
      }
      av_packet_unref(d->pkt);
    } else {
      // 如果返回值为 AVERROR(EAGAIN)(表示解码器暂时无法接受更多数据),则会设置 d->packet_pending 标志位为 1,表示这个数据包需要等待下次继续处理。
      if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
        // 解码器暂时无法接受更多数据,需要将数据包标记为待处理。
        av_log(d->avctx, AV_LOG_ERROR,
               "Receive_frame and send_packet both returned EAGAIN, which is "
               "an API violation.\n");
        d->packet_pending = 1;
      } else {
        av_packet_unref(d->pkt);
      }
    }
  }
}

static void decoder_destroy(Decoder *d) {
  av_packet_free(&d->pkt);
  avcodec_free_context(&d->avctx);
}

// 释放Frame⾥⾯的AVFrame和 AVSubtitle
static void frame_queue_unref_item(Frame *vp) {
  av_frame_unref(vp->frame);
  avsubtitle_free(&vp->sub);
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size,
                            int keep_last) {
  int i;
  memset(f, 0, sizeof(FrameQueue));
  if (!(f->mutex = SDL_CreateMutex())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  if (!(f->cond = SDL_CreateCond())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  f->pktq = pktq;
  f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
  // !!是为了将int类型转为bool
  f->keep_last = !!keep_last;
  for (i = 0; i < f->max_size; i++) {
    if (!(f->queue[i].frame = av_frame_alloc())) {
      return AVERROR(ENOMEM);
    }
  }
  return 0;
}

static void frame_queue_destroy(FrameQueue *f) {
  int i;
  for (i = 0; i < f->max_size; i++) {
    Frame *vp = &f->queue[i];
    frame_queue_unref_item(vp);
    av_frame_free(&vp->frame);
  }
  SDL_DestroyMutex(f->mutex);
  SDL_DestroyCond(f->cond);
}

// 发送条件信号，通知等待的线程
static void frame_queue_signal(FrameQueue *f) {
  SDL_LockMutex(f->mutex);
  SDL_CondSignal(f->cond);
  SDL_UnlockMutex(f->mutex);
}

// 返回当前读取位置的帧（不移除）
static Frame *frame_queue_peek(FrameQueue *f) {
  return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

// 返回下一读取位置的帧（不移除）
static Frame *frame_queue_peek_next(FrameQueue *f) {
  return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

/* ========= 获取last Frame，返回最后读取位置的帧 =========
 * 当rindex_shown=0时，和frame_queue_peek效果一样
 * 当rindex_shown=1时，读取的是已经显示过的frame
*/
static Frame *frame_queue_peek_last(FrameQueue *f) {
  return &f->queue[f->rindex];
}

// 等待直到有可写入的新帧空间，返回可写入帧的指针
static Frame *frame_queue_peek_writable(FrameQueue *f) {
  /* wait until we have space to put a new frame */
  SDL_LockMutex(f->mutex);
  while (f->size >= f->max_size && !f->pktq->abort_request) {
    SDL_CondWait(f->cond, f->mutex);
  }
  SDL_UnlockMutex(f->mutex);

  if (f->pktq->abort_request) {
    return NULL;
  }

  return &f->queue[f->windex];
}

// 等待直到有可读取的新帧，返回可读取帧的指针
static Frame *frame_queue_peek_readable(FrameQueue *f) {
  /* wait until we have a readable a new frame */
  SDL_LockMutex(f->mutex);
  while (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request) {
    SDL_CondWait(f->cond, f->mutex);
  }
  SDL_UnlockMutex(f->mutex);

  if (f->pktq->abort_request) {
    return NULL;
  }

  return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f) {
  if (++f->windex == f->max_size) {
    f->windex = 0;
  }
  SDL_LockMutex(f->mutex);
  f->size++;
  SDL_CondSignal(f->cond);
  SDL_UnlockMutex(f->mutex);
}

// 将读取位置推进到下一个位置，减少队列中的帧数，并发送条件信号。
// 更新读索引(同时删除旧frame)
static void frame_queue_next(FrameQueue *f) {
  if (f->keep_last && !f->rindex_shown) {
    f->rindex_shown = 1;
    return;
  }
  frame_queue_unref_item(&f->queue[f->rindex]);
  if (++f->rindex == f->max_size) {
    f->rindex = 0;
  }
  SDL_LockMutex(f->mutex);
  f->size--;
  SDL_CondSignal(f->cond);
  SDL_UnlockMutex(f->mutex);
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue *f) {
  return f->size - f->rindex_shown;
}

// 获取最近播放Frame对应数据在媒体⽂件的位置，主要在seek时使⽤
/* return last shown position */
static int64_t frame_queue_last_pos(FrameQueue *f) {
  Frame *fp = &f->queue[f->rindex];
  if (f->rindex_shown && fp->serial == f->pktq->serial)
    return fp->pos;
  else
    return -1;
}

// 中止解码器：当收到信号或触发某些条件时,decoder_abort 函数会被调用,用于通知解码器立即中止当前的解码操作。这可能发生在用户手动停止播放、切换媒体源或者出现解码错误等情况下。
static void decoder_abort(Decoder *d, FrameQueue *fq) {
  packet_queue_abort(d->queue);
  frame_queue_signal(fq);
  SDL_WaitThread(d->decoder_tid, NULL);
  d->decoder_tid = NULL;
  packet_queue_flush(d->queue);
}

static int realloc_texture(SDL_Texture **texture, Uint32 new_format,
                           int new_width, int new_height,
                           SDL_BlendMode blendmode, int init_texture) {
  Uint32 format;
  int access, w, h;
  // SDL_QueryTexture用于获取SDL纹理的基本属性信息，比如它的像素格式、访问模式、宽度和高度等。
  if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 ||
      new_width != w || new_height != h || new_format != format) {
    void *pixels;
    int pitch;
    if (*texture) SDL_DestroyTexture(*texture);
    if (!(*texture = SDL_CreateTexture(renderer, new_format,
                                       SDL_TEXTUREACCESS_STREAMING, new_width,
                                       new_height))) {
      return -1;
    }
    if (SDL_SetTextureBlendMode(*texture, blendmode) < 0) return -1;
    if (init_texture) {
      if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0) return -1;
      memset(pixels, 0, pitch * new_height);
      SDL_UnlockTexture(*texture);
    }
    av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width,
           new_height, SDL_GetPixelFormatName(new_format));
  }
  return 0;
}

// 根据给定的屏幕尺寸和视频尺寸,计算出一个合适的显示区域大小和位置
// 根据屏幕宽高和视频长宽，计算出视频在屏幕上的最大宽度或高度，并确保这个宽度值是一个偶数。
static void calculate_display_rect(SDL_Rect *rect, int scr_xleft, int scr_ytop,
                                   int scr_width, int scr_height, int pic_width,
                                   int pic_height, AVRational pic_sar) {
  AVRational aspect_ratio = pic_sar;
  int64_t width, height, x, y;

  // av_make_q根据给定的分子 num 和分母 den 创建一个 AVRational 类型的分数
  // av_cmp_q比较两个分数 a 和 b的大小
  if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
    aspect_ratio = av_make_q(1, 1);
  // av_mul_q将两个 AVRational 类型的分数相乘,并返回乘积
  aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

  /* XXX: we suppose the screen has a 1.0 pixel ratio */
  height = scr_height;
  width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
  if (width > scr_width) {
    width = scr_width;
    height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
  }
  /*
  & ~1:
  这个操作是为了将计算出的 width 值向下取整为一个偶数。
  原因是某些视频硬件或软件可能要求视频宽度必须是偶数，否则可能会出现显示问题。
  & ~1 的原理是将 width 值的最低位位置为 0，即取偶数。
*/
  x = (scr_width - width) / 2;
  y = (scr_height - height) / 2;
  rect->x = scr_xleft + x;
  rect->y = scr_ytop + y;
  rect->w = FFMAX((int)width, 1);
  rect->h = FFMAX((int)height, 1);
}

// 获取 SDL 的像素格式和混合模式
static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt,
                                          SDL_BlendMode *sdl_blendmode) {
  int i;
  *sdl_blendmode = SDL_BLENDMODE_NONE;
  *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
  if (format == AV_PIX_FMT_RGB32 || format == AV_PIX_FMT_RGB32_1 ||
      format == AV_PIX_FMT_BGR32 || format == AV_PIX_FMT_BGR32_1)
    *sdl_blendmode = SDL_BLENDMODE_BLEND;
  for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
    if (format == sdl_texture_format_map[i].format) {
      *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
      return;
    }
  }
}

/*
 根据 AVFrame 的像素格式获取对应的 SDL 像素格式和混合模式
 如果需要,会根据 AVFrame 的宽高重新分配 SDL_Texture 的大小。
 上传 YUV 数据
 上传其他格式数据
*/
static int upload_texture(SDL_Texture **tex, AVFrame *frame) {
  int ret = 0;
  Uint32 sdl_pix_fmt;
  SDL_BlendMode sdl_blendmode;
  get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
  if (realloc_texture(tex,
                      sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN
                          ? SDL_PIXELFORMAT_ARGB8888
                          : sdl_pix_fmt,
                      frame->width, frame->height, sdl_blendmode, 0) < 0) {
    return -1;
  }

  /*
  根据 SDL 的像素格式,采取不同的上传方式:
  如果是未知的像素格式,需要先用 sws_scale 函数进行颜色空间转换,然后再上传到纹理。
  如果是 SDL_PIXELFORMAT_IYUV(即 YUV 420P 格式),可以直接使用 SDL_UpdateYUVTexture 函数上传。
  对于其他像素格式,可以直接使用 SDL_UpdateTexture 函数上传。
  对于有负线宽的数据,需要特殊处理以确保正确上传。
  */
  switch (sdl_pix_fmt) {
    case SDL_PIXELFORMAT_UNKNOWN:
      /* This should only happen if we are not using avfilter... */
      //  *img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
      //      frame->width, frame->height, frame->format, frame->width, frame->height,
      //      AV_PIX_FMT_BGRA, sws_flags, NULL, NULL, NULL);
      //  if (*img_convert_ctx != NULL) {
      //      uint8_t *pixels[4];
      //      int pitch[4];
      //      if (!SDL_LockTexture(*tex, NULL, (void **)pixels, pitch)) {
      //          sws_scale(*img_convert_ctx, (const uint8_t * const *)frame->data, frame->linesize,
      //                    0, frame->height, pixels, pitch);
      //          SDL_UnlockTexture(*tex);
      //      }
      //  } else {
      //      av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
      //      ret = -1;
      //  }
      av_log(NULL, AV_LOG_ERROR,
             "Cannot initialize the conversion context when the pix_fmt is "
             "SDL_PIXELFORMAT_UNKNOWN.\n");
      break;
    case SDL_PIXELFORMAT_IYUV:
      if (frame->linesize[0] > 0 && frame->linesize[1] > 0 &&
          frame->linesize[2] > 0) {
        ret = SDL_UpdateYUVTexture(
            *tex, NULL, frame->data[0], frame->linesize[0], frame->data[1],
            frame->linesize[1], frame->data[2], frame->linesize[2]);
      } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 &&
                 frame->linesize[2] < 0) {
        ret = SDL_UpdateYUVTexture(
            *tex, NULL,
            frame->data[0] + frame->linesize[0] * (frame->height - 1),
            -frame->linesize[0],
            frame->data[1] +
                frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
            -frame->linesize[1],
            frame->data[2] +
                frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
            -frame->linesize[2]);
        // AV_CEIL_RSHIFT对一个数值进行向上取整的右移操作。最后的结果不是0就是1
      } else {
        av_log(NULL, AV_LOG_ERROR,
               "Mixed negative and positive linesizes are not supported.\n");
        return -1;
      }
      break;
    default:
      if (frame->linesize[0] < 0) {
        ret = SDL_UpdateTexture(
            *tex, NULL,
            frame->data[0] + frame->linesize[0] * (frame->height - 1),
            -frame->linesize[0]);
      } else {
        ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
      }
      break;
  }
  return ret;
}

// 这个函数允许开发者手动设置 SDL 的 YUV 转换模式。不同的转换模式会影响到视频颜色的显示效果。通过调整这个设置，可以优化视频的显示质量。
// static void set_sdl_yuv_conversion_mode(AVFrame *frame) {}

// 将一个 AVFrame 中的视频数据上传到一个 SDL_Texture 对象中
static void video_image_display(VideoState *is) {
  Frame *vp;
  Frame *sp = NULL;
  SDL_Rect rect;

  vp = frame_queue_peek_last(&is->pictq);
  if (is->subtitle_st) {
    if (frame_queue_nb_remaining(&is->subpq) > 0) {
      sp = frame_queue_peek(&is->subpq);

      if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000)) {
        if (!sp->uploaded) {
          uint8_t *pixels[4];
          int pitch[4];
          int i;
          if (!sp->width || !sp->height) {
            sp->width = vp->width;
            sp->height = vp->height;
          }
          if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888,
                              sp->width, sp->height, SDL_BLENDMODE_BLEND,
                              1) < 0) {
            return;
          }

          for (i = 0; i < sp->sub.num_rects; i++) {
            AVSubtitleRect *sub_rect = sp->sub.rects[i];

            sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
            sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
            sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
            sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

            is->sub_convert_ctx = sws_getCachedContext(
                is->sub_convert_ctx, sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA, 0, NULL, NULL, NULL);
            if (!is->sub_convert_ctx) {
              av_log(NULL, AV_LOG_FATAL,
                     "Cannot initialize the conversion context\n");
              return;
            }
            if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect,
                                 (void **)pixels, pitch)) {
              sws_scale(is->sub_convert_ctx,
                        (const uint8_t *const *)sub_rect->data,
                        sub_rect->linesize, 0, sub_rect->h, pixels, pitch);
              SDL_UnlockTexture(is->sub_texture);
            }
          }
          sp->uploaded = 1;
        }
      } else
        sp = NULL;
    }
  }

  calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height,
                         vp->width, vp->height, vp->sar);

  if (!vp->uploaded) {
    if (upload_texture(&is->vid_texture, vp->frame) < 0) {
      return;
    }
    vp->uploaded = 1;
    vp->flip_v = vp->frame->linesize[0] < 0;
  }

  SDL_RenderCopyEx(renderer, is->vid_texture, NULL, &rect, 0, NULL,
                   vp->flip_v ? SDL_FLIP_VERTICAL : 0);
  if (sp) {
    // 字幕一次性渲染出来
    SDL_RenderCopy(renderer, is->sub_texture, NULL, &rect);
  }
}

/*
 负责在视频播放过程中显示音频波形图或频谱分析图的。它主要执行以下功能:
 根据用户选择的显示模式(波形图或频谱分析图)，使用不同的方式来绘制音频数据
 暂时不要
*/
static inline int compute_mod(int a, int b) {}
static void display_by_mode(VideoState *s) {}
static void video_audio_display(VideoState *s) {}

static void stream_component_close(VideoState *is, int stream_index) {
  AVFormatContext *ic = is->ic;
  AVCodecParameters *codecpar;

  if (stream_index < 0 || stream_index >= ic->nb_streams) {
    return;
  }
  codecpar = ic->streams[stream_index]->codecpar;

  switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      decoder_abort(&is->auddec, &is->sampq);
      SDL_CloseAudioDevice(audio_dev);
      decoder_destroy(&is->auddec);
      swr_free(&is->swr_ctx);
      av_freep(&is->audio_buf1);
      is->audio_buf1_size = 0;
      is->audio_buf = NULL;
      break;
    case AVMEDIA_TYPE_VIDEO:
      decoder_abort(&is->viddec, &is->pictq);
      decoder_destroy(&is->viddec);
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      decoder_abort(&is->subdec, &is->subpq);
      decoder_destroy(&is->subdec);
      break;
    default:
      break;
  }

  // 控制流的丢弃策略
  // AVDISCARD_ALL 代表完全丢弃该流,不会对其进行任何解码和处理。
  ic->streams[stream_index]->discard = AVDISCARD_ALL;
  switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      is->audio_st = NULL;
      is->audio_stream = -1;
      break;
    case AVMEDIA_TYPE_VIDEO:
      is->video_st = NULL;
      is->video_stream = -1;
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      is->subtitle_st = NULL;
      is->subtitle_stream = -1;
      break;
    default:
      break;
  }
}

static void stream_close(VideoState *is) {
  /* XXX: use a special url_shutdown call to abort parse cleanly */
  is->abort_request = 1;
  SDL_WaitThread(is->read_tid, NULL);

  /* close each stream */
  if (is->audio_stream >= 0) stream_component_close(is, is->audio_stream);
  if (is->video_stream >= 0) stream_component_close(is, is->video_stream);
  if (is->subtitle_stream >= 0) stream_component_close(is, is->subtitle_stream);

  avformat_close_input(&is->ic);

  packet_queue_destroy(&is->videoq);
  packet_queue_destroy(&is->audioq);
  packet_queue_destroy(&is->subtitleq);

  /* free all pictures */
  frame_queue_destroy(&is->pictq);
  frame_queue_destroy(&is->sampq);
  frame_queue_destroy(&is->subpq);
  SDL_DestroyCond(is->continue_read_thread);
  sws_freeContext(is->sub_convert_ctx);
  av_free(is->filename);
  if (is->vid_texture) SDL_DestroyTexture(is->vid_texture);
  if (is->sub_texture) SDL_DestroyTexture(is->sub_texture);
  av_free(is);
}

static void do_exit(VideoState *is) {
  if (is) {
    stream_close(is);
  }
  if (renderer) SDL_DestroyRenderer(renderer);
  if (window) SDL_DestroyWindow(window);
  /*uninit_opts();*/
  av_freep(&input_filename);
  avformat_network_deinit();
  if (show_status) {
    printf("\n");
  }
  SDL_Quit();
  av_log(NULL, AV_LOG_QUIET, "%s", "");
  exit(0);
}

static void sigterm_handler(int sig) {
  /*
     终止当前进程的函数调用。
     当调用 exit() 函数时,进程会以传入的参数值(这里是 1001)作为退出状态码退出。
     退出状态码是一个整数值,通常用于表示进程的退出状态。零表示正常退出,非零表示异常退出。
     在 Unix/Linux 系统中,退出状态码可以被父进程或操作系统读取,用于判断子进程的执行结果。
    */
  exit(1001);
}

static void set_default_window_size(int width, int height,
                                    AVRational aspect_ratio) {
  SDL_Rect rect;
  int max_width = screen_width ? screen_width : INT_MAX;
  int max_height = screen_height ? screen_height : INT_MAX;
  if (max_width == INT_MAX && max_height == INT_MAX) {
    max_height = height;
  }
  calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height,
                         aspect_ratio);
  default_width = rect.w;
  default_height = rect.h;
}

static int video_open(VideoState *is) {
  int w, h;

  w = screen_width ? screen_width : default_width;
  h = screen_height ? screen_height : default_height;

  if (!window_title) {
    window_title = input_filename;
  }
  SDL_SetWindowTitle(window, window_title);

  SDL_SetWindowSize(window, w, h);
  SDL_SetWindowPosition(window, screen_left, screen_top);
  if (is_full_screen)
    SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_ShowWindow(window);

  is->width = w;
  is->height = h;

  return 0;
}

/* display the current picture, if any */
static void video_display(VideoState *is) {
  if (!is->width) {
    video_open(is);
  }

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO) {
    // video_audio_display(is);  // 根据显示模式显示波形图或频谱分析图，先不处理
  } else if (is->video_st) {
    video_image_display(is);
  }
  SDL_RenderPresent(renderer);
}

// 用于获取当前时钟的时间值
static double get_clock(Clock *c) {
  if (*c->queue_serial != c->serial) return NAN;
  if (c->paused) {
    return c->pts;
  } else {
    double time = av_gettime_relative() / 1000000.0;
    return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
  }
}

// 设置时钟 c 的当前时间为 pts，并将时钟的 serial 编号更新为指定值。
// 在收到新的时间戳信息时更新时钟,确保时钟时间与视频/音频数据同步。
static void set_clock_at(Clock *c, double pts, int serial, double time) {
  c->pts = pts;
  c->last_updated = time;
  c->pts_drift = c->pts - time;
  c->serial = serial;
}

// 用于设置当前时钟的时间值。
static void set_clock(Clock *c, double pts, int serial) {
  double time = av_gettime_relative() / 1000000.0;
  set_clock_at(c, pts, serial, time);
}

// 设置时钟 c 的播放速率为 speed。
// 主要用于同步音视频时钟,而不是用于实现倍速播放。
static void set_clock_speed(Clock *c, double speed) {
  set_clock(c, get_clock(c), c->serial);
  c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial) {
  c->speed = 1.0;
  c->paused = 0;
  c->queue_serial = queue_serial;
  set_clock(c, NAN, -1);  // NAN: 表示无法用正常数值表示的情况，不代表任何数值
}

// 将时钟 c 与 slave 时钟同步。
// 用于在多路流播放时,将视频时钟与音频时钟进行同步。确保视频和音频保持良好的同步。
static void sync_clock_to_slave(Clock *c, Clock *slave) {
  double clock = get_clock(c);
  double slave_clock = get_clock(slave);
  if (!isnan(slave_clock) &&
      (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD)) {
    set_clock(c, slave_clock, slave->serial);
  }
}

static int get_master_sync_type(VideoState *is) {
  if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
    if (is->video_st)
      return AV_SYNC_VIDEO_MASTER;
    else
      return AV_SYNC_AUDIO_MASTER;
  } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
    if (is->audio_st)
      return AV_SYNC_AUDIO_MASTER;
    else
      return AV_SYNC_EXTERNAL_CLOCK;
  } else {
    return AV_SYNC_EXTERNAL_CLOCK;
  }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is) {
  double val;

  switch (get_master_sync_type(is)) {
    case AV_SYNC_VIDEO_MASTER:
      val = get_clock(&is->vidclk);
      break;
    case AV_SYNC_AUDIO_MASTER:
      val = get_clock(&is->audclk);
      break;
    default:
      val = get_clock(&is->extclk);
      break;
  }
  return val;
}

// 动态调整外部时钟的速度
// 根据视频和音频队列中的数据包数量来动态调整外部时钟的速度,以保持播放的同步和流畅。
static void check_external_clock_speed(VideoState *is) {
  if (is->video_stream >= 0 &&
          is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
      is->audio_stream >= 0 &&
          is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
    // 时钟速度需要降低以防止队列中的数据包耗尽
    set_clock_speed(&is->extclk,
                    FFMAX(EXTERNAL_CLOCK_SPEED_MIN,
                          is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
  } else if ((is->video_stream < 0 ||
              is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
             (is->audio_stream < 0 ||
              is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
    // 时钟速度需要增加以防止队列中的数据包过多
    set_clock_speed(&is->extclk,
                    FFMIN(EXTERNAL_CLOCK_SPEED_MAX,
                          is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
  } else {
    // 如果上述两种情况都不满足,则根据 extclk 的当前速度做一个平滑的调整,使其逐渐接近 1.0。这种情况下,说明队列中的数据包数量处于合理范围内,不需要大幅调整时钟速度
    double speed = is->extclk.speed;
    if (speed != 1.0)
      set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP *
                                               (1.0 - speed) /
                                               fabs(1.0 - speed));
  }
}

/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel,
                        int by_bytes) {
  if (!is->seek_req) {
    is->seek_pos = pos;
    is->seek_rel = rel;
    is->seek_flags &= ~AVSEEK_FLAG_BYTE;
    if (by_bytes) {
      is->seek_flags |= AVSEEK_FLAG_BYTE;
    }
    is->seek_req = 1;
    SDL_CondSignal(is->continue_read_thread);
  }
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is) {
  if (is->paused) {
    is->frame_timer +=
        av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
    if (is->read_pause_return != AVERROR(ENOSYS)) {
      is->vidclk.paused = 0;
    }
    set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
  }
  set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
  // 将 paused 标志应用到所有相关时钟
  is->paused = !is->paused;
  is->audclk.paused = !is->paused;
  is->vidclk.paused = !is->paused;
  is->extclk.paused = !is->paused;
}

static void toggle_pause(VideoState *is) {
  stream_toggle_pause(is);
  is->step = 0;
}

static void toggle_mute(VideoState *is) { is->muted = !is->muted; }

// 这里涉及到一些数学运算
// sign 参数: 这个参数用于控制音量的增加或减少方向。
// step 参数: 这个参数用于控制每次音量调整的步长大小。
static void update_volume(VideoState *is, int sign, double step) {
  /*
   lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
   这行代码的作用是根据当前的音量级别和调整步长,计算出新的音量值。
   具体步骤如下:
   volume_level + sign * step
   这一步计算出新的音量级别。
   如果 sign 为 1,表示要增加音量,则加上 step。
   如果 sign 为 -1,表示要降低音量,则减去 step。
   
   pow(10.0, (volume_level + sign * step) / 20.0)
   这一步将新的音量级别转换为0到1之间的比例值。
   
   公式 10^((volume_level + sign * step) / 20) 是将音量级别(以分贝为单位)转换为0到1之间的比例值。
   SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0)
   这一步将比例值转换为SDL音量范围(0到SDL_MIX_MAXVOLUME)内的实际值。
   也就是说,将新计算出的比例值乘以SDL_MIX_MAXVOLUME,得到新的音量值。
   
   lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0))
   最后, 使用lrint()函数将计算出的浮点数音量值四舍五入为整数, 因为音量值需要是整数。
  */

  // 计算出当前的音量级别
  double volume_level =
      is->audio_volume
          ? (20 * log(is->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10))
          : -1000.0;
  // 根据 sign 和 step 参数, 计算出新的音量级别
  int new_volume =
      lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
  // 将 is->audio_volume 限制在 0 到 SDL_MIX_MAXVOLUME 的范围内
  is->audio_volume = av_clip(
      is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume,
      0, SDL_MIX_MAXVOLUME);
}

static void step_to_next_frame(VideoState *is) {
  /* if the stream is paused unpause it, then step */
  if (is->paused) {
    stream_toggle_pause(is);
  }
  is->step = 1;
}

// 根据当前的同步状态,计算出视频帧需要的延迟时间
static double compute_target_delay(double delay, VideoState *is) {
  double sync_threshold, diff = 0;

  /* update delay to follow master synchronisation source */
  if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
    /* if video is slave, we try to correct big delays by
            duplicating or deleting a frame */
    /* 根据diff决定是需要跳过还是重复播放某些帧 */
    diff = get_clock(&is->vidclk) - get_master_clock(is);

    /* skip or repeat frame. We take into account the
            delay to compute the threshold. I still don't know
            if it is the best guess */
    /* 
          如果 diff 小于 -sync_threshold, 增加 delay 时间。
          如果 diff 大于 sync_threshold 且 delay 大于 AV_SYNC_FRAMEDUP_THRESHOLD, 增加 delay 时间。
          如果 diff 大于 sync_threshold, 将 delay 时间加倍。
    */
    sync_threshold =
        FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
    if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
      if (diff <= -sync_threshold)
        delay = FFMAX(0, delay + diff);
      else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
        delay = delay + diff;
      else if (diff >= sync_threshold)
        delay = 2 * delay;
    }
  }

  av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);

  return delay;
}

// 获取两个连续视频帧之间的时间间隔
static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
  //检查两个视频帧是否属于同一个序列
  if (vp->serial == nextvp->serial) {
    double duration = nextvp->pts - vp->pts;
    if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
      return vp->duration;
    else
      return duration;
  } else {
    return 0.0;
  }
}

static void update_video_pts(VideoState *is, double pts, int serial) {
  /* update current video pts */
  set_clock(&is->vidclk, pts, serial);
  sync_clock_to_slave(&is->extclk, &is->vidclk);
}

// https://blog.csdn.net/weixin_41910694/article/details/115795452 --有相关说明
/* called to display each frame */
static void video_refresh(void *opaque, double *remaining_time) {
  VideoState *is = opaque;
  double time;

  Frame *sp, *sp2;

  if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK &&
      is->realtime) {
    check_external_clock_speed(is);
  }

  /* 
     写队列中，应⽤程序写⼊⼀个新帧后通常总是将写索引加1。
     ⽽读队列中，“读取”和“更新读索引(同时删除旧帧)”⼆者是独⽴的，可以只读取⽽不更新读索引，
     也可以只更新读索引(只删除)⽽不读取（只有更新读索引的时候才真正释放对应的Frame数据）。⽽且读队列引⼊了是否保留已显示的最后⼀帧的机制，导致读队列⽐写队列要复杂很多。
     读队列和写队列步骤是类似的，基本步骤如下：
     调⽤frame_queue_peek_readable获取可读Frame；
     如果需要更新读索引rindex（出队列该节点）则调⽤frame_queue_peek_next；
    */
  if (is->video_st) {
    while (1) {
      if (frame_queue_nb_remaining(&is->pictq) == 0) {
        // nothing to do, no picture to display in the queue
        break;
      } else {
        double last_duration, duration, delay;
        Frame *vp, *lastvp;

        /* dequeue the picture */
        lastvp = frame_queue_peek_last(&is->pictq);
        vp = frame_queue_peek(&is->pictq);

        if (vp->serial != is->videoq.serial) {
          frame_queue_next(&is->pictq);
          continue;
        }

        if (lastvp->serial != vp->serial)
          is->frame_timer = av_gettime_relative() / 1000000.0;

        if (is->paused) {
          /* display picture */
          if (!display_disable && is->force_refresh &&
              is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown) {
            video_display(is);
          }
          break;
        }

        last_duration = vp_duration(is, lastvp, vp);
        delay = compute_target_delay(last_duration, is);

        time = av_gettime_relative() / 1000000.0;
        if (time < is->frame_timer + delay) {
          *remaining_time =
              FFMIN(is->frame_timer + delay - time, *remaining_time);
          /* display picture */
          if (!display_disable && is->force_refresh &&
              is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
            video_display(is);
          break;
        }

        is->frame_timer += delay;
        if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
          is->frame_timer = time;

        SDL_LockMutex(is->pictq.mutex);
        if (!isnan(vp->pts)) {
          update_video_pts(is, vp->pts, vp->serial);
        }
        SDL_UnlockMutex(is->pictq.mutex);

        if (frame_queue_nb_remaining(&is->pictq) > 1) {
          Frame *nextvp = frame_queue_peek_next(&is->pictq);
          duration = vp_duration(is, vp, nextvp);
          if (!is->step &&
              (framedrop > 0 || (framedrop && get_master_sync_type(is) !=
                                                  AV_SYNC_VIDEO_MASTER)) &&
              time > is->frame_timer + duration) {
            is->frame_drops_late++;
            frame_queue_next(&is->pictq);
            continue;
          }
        }

        if (is->subtitle_st) {
          while (frame_queue_nb_remaining(&is->subpq) > 0) {
            sp = frame_queue_peek(&is->subpq);

            if (frame_queue_nb_remaining(&is->subpq) > 1)
              sp2 = frame_queue_peek_next(&is->subpq);
            else
              sp2 = NULL;

            if (sp->serial != is->subtitleq.serial ||
                (is->vidclk.pts >
                 (sp->pts + ((float)sp->sub.end_display_time / 1000))) ||
                (sp2 && is->vidclk.pts >
                            (sp2->pts +
                             ((float)sp2->sub.start_display_time / 1000)))) {
              if (sp->uploaded) {
                int i;
                for (i = 0; i < sp->sub.num_rects; i++) {
                  AVSubtitleRect *sub_rect = sp->sub.rects[i];
                  uint8_t *pixels;
                  int pitch, j;

                  if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect,
                                       (void **)&pixels, &pitch)) {
                    for (j = 0; j < sub_rect->h; j++, pixels += pitch)
                      memset(pixels, 0, sub_rect->w << 2);
                    SDL_UnlockTexture(is->sub_texture);
                  }
                }
              }
              frame_queue_next(&is->subpq);
            } else {
              break;
            }
          }
        }

        frame_queue_next(&is->pictq);
        is->force_refresh = 1;

        if (is->step && !is->paused) {
          stream_toggle_pause(is);
        }
      }

      /* display picture */
      if (!display_disable && is->force_refresh &&
          is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown) {
        video_display(is);
      }
      break;
    }
    is->force_refresh = 0;
    if (show_status) {
      AVBPrint buf;
      static int64_t last_time;
      int64_t cur_time;
      int aqsize, vqsize, sqsize;
      double av_diff;

      cur_time = av_gettime_relative();
      if (!last_time || (cur_time - last_time) >= 30000) {
        aqsize = 0;
        vqsize = 0;
        sqsize = 0;
        if (is->audio_st) aqsize = is->audioq.size;
        if (is->video_st) vqsize = is->videoq.size;
        if (is->subtitle_st) sqsize = is->subtitleq.size;
        av_diff = 0;
        if (is->audio_st && is->video_st)
          av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
        else if (is->video_st)
          av_diff = get_master_clock(is) - get_clock(&is->vidclk);
        else if (is->audio_st)
          av_diff = get_master_clock(is) - get_clock(&is->audclk);

        av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
        av_bprintf(
            &buf, "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB \r",
            get_master_clock(is),
            (is->audio_st && is->video_st)
                ? "A-V"
                : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
            av_diff, is->frame_drops_early + is->frame_drops_late,
            aqsize / 1024, vqsize / 1024, sqsize);

        if (show_status == 1 && AV_LOG_INFO > av_log_get_level())
          fprintf(stderr, "%s", buf.str);
        else
          av_log(NULL, AV_LOG_INFO, "%s", buf.str);

        fflush(stderr);
        av_bprint_finalize(&buf, NULL);

        last_time = cur_time;
      }
    }
  }
}

// frame 入队列之前的初始化赋值处理
static int queue_picture(VideoState *is, AVFrame *src_frame, double pts,
                         double duration, int64_t pos, int serial) {
  Frame *vp;

  // 用于获取视频帧的帧类型
  //  printf("frame_type=%c pts=%0.3f\n",
  //         av_get_picture_type_char(src_frame->pict_type), pts);

  if (!(vp = frame_queue_peek_writable(&is->pictq))) {
    return -1;
  }

  vp->sar = src_frame->sample_aspect_ratio;
  vp->uploaded = 0;

  vp->width = src_frame->width;
  vp->height = src_frame->height;
  vp->format = src_frame->format;

  vp->pts = pts;
  vp->duration = duration;
  vp->pos = pos;
  vp->serial = serial;

  set_default_window_size(vp->width, vp->height, vp->sar);
  // 将src中所有数据拷贝到dst中，并复位src。
  av_frame_move_ref(vp->frame, src_frame);
  frame_queue_push(&is->pictq);
  return 0;
}

// 获取视频帧
static int get_video_frame(VideoState *is, AVFrame *frame) {
  int got_picture;

  if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
    return -1;

  if (got_picture) {
    double dpts = NAN;

    if (frame->pts != AV_NOPTS_VALUE)
      dpts = av_q2d(is->video_st->time_base) * frame->pts;

    frame->sample_aspect_ratio =
        av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

    if (framedrop > 0 ||
        (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
      if (frame->pts != AV_NOPTS_VALUE) {
        double diff = dpts - get_master_clock(is);
        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD && diff < 0 &&
            /*diff - is->frame_last_filter_delay < 0 &&*/
            is->viddec.pkt_serial == is->vidclk.serial &&
            is->videoq.nb_packets) {
          is->frame_drops_early++;
          av_frame_unref(frame);
          got_picture = 0;
        }
      }
    }
  }

  return got_picture;
}

static int audio_thread(void *arg) {
  VideoState *is = arg;
  AVFrame *frame = av_frame_alloc();
  Frame *af;
  int got_frame = 0;
  AVRational tb;
  int ret = 0;

  if (!frame) {
    return AVERROR(ENOMEM);
  }

  do {
    if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0) {
      av_frame_free(&frame);
      return ret;
    }

    if (got_frame) {
      tb = (AVRational){1, frame->sample_rate};

      if (!(af = frame_queue_peek_writable(&is->sampq))) {
        av_frame_free(&frame);
        return ret;
      }

      af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
      af->pos = frame->pkt_pos;
      af->serial = is->auddec.pkt_serial;
      af->duration =
          av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

      av_frame_move_ref(af->frame, frame);
      frame_queue_push(&is->sampq);
    }
  } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
  av_frame_free(&frame);
  return ret;
}

static int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name,
                         void *arg) {
  packet_queue_start(d->queue);
  d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
  if (!d->decoder_tid) {
    av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  return 0;
}

static int video_thread(void *arg) {
  VideoState *is = arg;
  AVFrame *frame = av_frame_alloc();
  double pts;
  double duration;
  int ret;
  AVRational tb = is->video_st->time_base;
  AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

  if (!frame) {
    return AVERROR(ENOMEM);
  }

  for (;;) {
    ret = get_video_frame(is, frame);
    if (ret < 0) {
      av_frame_free(&frame);
      return ret;
    }
    if (!ret) continue;

    duration = (frame_rate.num && frame_rate.den
                    ? av_q2d((AVRational){frame_rate.den, frame_rate.num})
                    : 0);
    pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
    ret = queue_picture(is, frame, pts, duration, frame->pkt_pos,
                        is->viddec.pkt_serial);
    av_frame_unref(frame);

    if (ret < 0) {
      av_frame_free(&frame);
      return 0;
    }
  }
  av_frame_free(&frame);
  return 0;
}

static int subtitle_thread(void *arg) {
  VideoState *is = arg;
  Frame *sp;
  int got_subtitle;
  double pts;

  for (;;) {
    if (!(sp = frame_queue_peek_writable(&is->subpq))) {
      return 0;
    }

    if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0)
      break;

    pts = 0;

    if (got_subtitle && sp->sub.format == 0) {
      if (sp->sub.pts != AV_NOPTS_VALUE)
        pts = sp->sub.pts / (double)AV_TIME_BASE;
      sp->pts = pts;
      sp->serial = is->subdec.pkt_serial;
      sp->width = is->subdec.avctx->width;
      sp->height = is->subdec.avctx->height;
      sp->uploaded = 0;

      /* now we can update the picture count */
      frame_queue_push(&is->subpq);
    } else if (got_subtitle) {
      avsubtitle_free(&sp->sub);
    }
  }
  return 0;
}

// 不进行音频内容显示，这个可以去掉了
/* copy samples for viewing in editor window */
// static void update_sample_display(VideoState *is, short *samples,
//                                   int samples_size) {
//   int size, len;
//   size = samples_size / sizeof(short);
//   while (size > 0) {
//     len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
//     if (len > size) {
//       len = size;
//     }
//     memcpy(is->sample_array + is->sample_array_index, samples,
//            len * sizeof(short));
//     samples += len;
//     is->sample_array_index += len;
//     if (is->sample_array_index >= SAMPLE_ARRAY_SIZE) {
//       is->sample_array_index = 0;
//     }
//     size -= len;
//   }
// }

/* return the wanted number of samples to get better sync if sync_type is video
  * or external master clock */
static int synchronize_audio(VideoState *is, int nb_samples) {
  int wanted_nb_samples = nb_samples;

  /* if not master, then we try to remove or add samples to correct the clock */
  if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
    double diff, avg_diff;
    int min_nb_samples, max_nb_samples;

    diff = get_clock(&is->audclk) - get_master_clock(is);

    if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
      is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
      if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
        /* not enough measures to have a correct estimate */
        is->audio_diff_avg_count++;
      } else {
        /* estimate the A-V difference */
        avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

        if (fabs(avg_diff) >= is->audio_diff_threshold) {
          wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
          min_nb_samples =
              ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          max_nb_samples =
              ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          wanted_nb_samples =
              av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
        }
        av_log(NULL, AV_LOG_TRACE,
               "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n", diff,
               avg_diff, wanted_nb_samples - nb_samples, is->audio_clock,
               is->audio_diff_threshold);
      }
    } else {
      /* too big difference : may be initial PTS errors, so
                reset A-V filter */
      is->audio_diff_avg_count = 0;
      is->audio_diff_cum = 0;
    }
  }

  return wanted_nb_samples;
}

/**
  * Decode one audio frame and return its uncompressed size.
  *
  * The processed audio frame is decoded, converted if required, and
  * stored in is->audio_buf, with size in bytes given by the return
  * value.
  */
static int audio_decode_frame(VideoState *is) {
  int data_size, resampled_data_size;
  av_unused double audio_clock0;
  int wanted_nb_samples;
  Frame *af;

  if (is->paused) {
    return -1;
  }

  do {
    // ffplay针对 32位的系统做的优化，避免由于硬件性能问题导致获取数据延迟更长时间，硬件差的环境在临界时间点不要轻易返回静音数据，延迟音频帧的播放。
#if defined(_WIN32)
    while (frame_queue_nb_remaining(&is->sampq) == 0) {
      if ((av_gettime_relative() - audio_callback_time) >
          1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2) {
        return -1;
      }

      av_usleep(1000);
    }
#endif
    if (!(af = frame_queue_peek_readable(&is->sampq))) {
      return -1;
    }
    frame_queue_next(&is->sampq);
  } while (af->serial != is->audioq.serial);

  // av_samples_get_buffer_size根据给定的音频参数(采样率、声道数、样本格式等)计算出所需的缓冲区大小(以字节为单位)
  data_size =
      av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
                                 af->frame->nb_samples, af->frame->format, 1);

  wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

  // av_channel_layout_compare 比较两个音频通道布局
  // 0 if chl and chl1 are equal, 1 if they are not equal. A negative AVERROR code if one or both are invalid.
  if (af->frame->format != is->audio_src.fmt ||
      av_channel_layout_compare(&af->frame->ch_layout,
                                &is->audio_src.ch_layout) ||
      af->frame->sample_rate != is->audio_src.freq ||
      (wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx)) {
    swr_free(&is->swr_ctx);
    // 设置重采样上下文的选项
    swr_alloc_set_opts2(&is->swr_ctx, &is->audio_tgt.ch_layout,
                        is->audio_tgt.fmt, is->audio_tgt.freq,
                        &af->frame->ch_layout, af->frame->format,
                        af->frame->sample_rate, 0, NULL);
    if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
      av_log(NULL, AV_LOG_ERROR,
             "Cannot create sample rate converter for conversion of %d Hz %s "
             "%d channels to %d Hz %s %d channels!\n",
             af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format),
             af->frame->ch_layout.nb_channels, is->audio_tgt.freq,
             av_get_sample_fmt_name(is->audio_tgt.fmt),
             is->audio_tgt.ch_layout.nb_channels);
      swr_free(&is->swr_ctx);
      return -1;
    }
    if (av_channel_layout_copy(&is->audio_src.ch_layout,
                               &af->frame->ch_layout) < 0)
      return -1;
    is->audio_src.freq = af->frame->sample_rate;
    is->audio_src.fmt = af->frame->format;
  }

  if (is->swr_ctx) {
    const uint8_t **in = (const uint8_t **)af->frame->extended_data;
    uint8_t **out = &is->audio_buf1;
    int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq /
                        af->frame->sample_rate +
                    256;
    int out_size =
        av_samples_get_buffer_size(NULL, is->audio_tgt.ch_layout.nb_channels,
                                   out_count, is->audio_tgt.fmt, 0);
    int len2;
    if (out_size < 0) {
      av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
      return -1;
    }
    if (wanted_nb_samples != af->frame->nb_samples) {
      // 在音频重采样过程中,由于采样率不同,可能会产生微小的时间偏差。
      // swr_set_compensation 函数可以设置补偿这种时间偏差,以确保重采样的精度。
      if (swr_set_compensation(is->swr_ctx,
                               (wanted_nb_samples - af->frame->nb_samples) *
                                   is->audio_tgt.freq / af->frame->sample_rate,
                               wanted_nb_samples * is->audio_tgt.freq /
                                   af->frame->sample_rate) < 0) {
        av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
        return -1;
      }
    }
    // 高效分配内存
    av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
    if (!is->audio_buf1) {
      return AVERROR(ENOMEM);
    }
    len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
    if (len2 < 0) {
      av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
      return -1;
    }
    if (len2 == out_count) {
      av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
      if (swr_init(is->swr_ctx) < 0) {
        swr_free(&is->swr_ctx);
      }
    }
    is->audio_buf = is->audio_buf1;
    resampled_data_size = len2 * is->audio_tgt.ch_layout.nb_channels *
                          av_get_bytes_per_sample(is->audio_tgt.fmt);
  } else {
    is->audio_buf = af->frame->data[0];
    resampled_data_size = data_size;
  }

  audio_clock0 = is->audio_clock;
  /* update the audio clock with the pts */
  if (!isnan(af->pts))
    is->audio_clock =
        af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
  else
    is->audio_clock = NAN;
  is->audio_clock_serial = af->serial;
#ifdef DEBUG
  {
    static double last_clock;
    printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
           is->audio_clock - last_clock, is->audio_clock, audio_clock0);
    last_clock = is->audio_clock;
  }
#endif
  return resampled_data_size;
}

/* prepare a new audio buffer */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
  VideoState *is = opaque;
  int audio_size, len1;

  audio_callback_time = av_gettime_relative();

  while (len > 0) {
    if (is->audio_buf_index >= is->audio_buf_size) {
      audio_size = audio_decode_frame(is);
      if (audio_size < 0) {
        /* if error, just output silence */
        is->audio_buf = NULL;
        is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE /
                             is->audio_tgt.frame_size *
                             is->audio_tgt.frame_size;
      } else {
        // if (is->show_mode != SHOW_MODE_VIDEO)
        //     update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
        is->audio_buf_size = audio_size;
      }
      is->audio_buf_index = 0;
    }
    len1 = is->audio_buf_size - is->audio_buf_index;
    if (len1 > len) {
      len1 = len;
    }
    if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
      memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
    else {
      memset(stream, 0, len1);
      if (!is->muted && is->audio_buf)
        SDL_MixAudioFormat(stream,
                           (uint8_t *)is->audio_buf + is->audio_buf_index,
                           AUDIO_S16SYS, len1, is->audio_volume);
    }
    len -= len1;
    stream += len1;
    is->audio_buf_index += len1;
  }
  is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
  /* Let's assume the audio driver that is used by SDL has two periods. */
  if (!isnan(is->audio_clock)) {
    set_clock_at(&is->audclk,
                 is->audio_clock - (double)(2 * is->audio_hw_buf_size +
                                            is->audio_write_buf_size) /
                                       is->audio_tgt.bytes_per_sec,
                 is->audio_clock_serial, audio_callback_time / 1000000.0);
    sync_clock_to_slave(&is->extclk, &is->audclk);
  }
}

// 配置和打开音频设备并设置音频参数，以便进行音频播放
static int audio_open(void *opaque, AVChannelLayout *wanted_channel_layout,
                      int wanted_sample_rate,
                      struct AudioParams *audio_hw_params) {
  SDL_AudioSpec wanted_spec, spec;
  const char *env;
  static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
  static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
  // #define FF_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
  // FF_ARRAY_ELEMS计算数组的大小(元素个数)。
  int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
  int wanted_nb_channels = wanted_channel_layout->nb_channels;

  // env = SDL_getenv("SDL_AUDIO_CHANNELS");
  // if (env) {
  //   wanted_nb_channels = atoi(env);
  //   av_channel_layout_uninit(wanted_channel_layout);
  //   av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
  // }
  // AV_CHANNEL_ORDER_NATIVE 表示音频通道布局采用本地系统的默认顺序。
  // 确保采用本地系统的默认通道顺序。为了确保音频通道的排列顺序与系统默认的一致，从而能够正确地播放音频。
  // 使用本地系统的默认通道顺序可以避免一些由于通道顺序不匹配而导致的音频播放问题。
  if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
    av_channel_layout_uninit(wanted_channel_layout);
    av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
  }
  wanted_nb_channels = wanted_channel_layout->nb_channels;
  wanted_spec.channels = wanted_nb_channels;
  wanted_spec.freq = wanted_sample_rate;
  if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
    av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
    return -1;
  }
  while (next_sample_rate_idx &&
         next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq) {
    next_sample_rate_idx--;
  }

  wanted_spec.format = AUDIO_S16SYS;
  wanted_spec.silence = 0;
  wanted_spec.samples =
      FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE,
            2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
  wanted_spec.callback = sdl_audio_callback;
  wanted_spec.userdata = opaque;
  // 如果失败，则调整通道数和采样率继续尝试，直到成功或没有更多组合可尝试，提高音频设备的兼容性
  // SDL_AUDIO_ALLOW_FREQUENCY_CHANGE音频设备无法支持所需的采样率时,SDL 会允许尝试使用最接近的可用采样率。
  // SDL_AUDIO_ALLOW_CHANNELS_CHANGE当音频设备无法支持所需的通道数时,SDL 会允许尝试使用最接近的可用通道数。
  while (
      !(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec,
                                        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                            SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
    av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
           wanted_spec.channels, wanted_spec.freq, SDL_GetError());
    wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
    if (!wanted_spec.channels) {
      wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
      wanted_spec.channels = wanted_nb_channels;
      if (!wanted_spec.freq) {
        av_log(NULL, AV_LOG_ERROR,
               "No more combinations to try, audio open failed\n");
        return -1;
      }
    }
    av_channel_layout_default(wanted_channel_layout, wanted_spec.channels);
  }
  if (spec.format != AUDIO_S16SYS) {
    av_log(NULL, AV_LOG_ERROR,
           "SDL advised audio format %d is not supported!\n", spec.format);
    return -1;
  }
  if (spec.channels != wanted_spec.channels) {
    av_channel_layout_uninit(wanted_channel_layout);
    av_channel_layout_default(wanted_channel_layout, spec.channels);
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
      av_log(NULL, AV_LOG_ERROR,
             "SDL advised channel count %d is not supported!\n", spec.channels);
      return -1;
    }
  }

  audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
  audio_hw_params->freq = spec.freq;
  if (av_channel_layout_copy(&audio_hw_params->ch_layout,
                             wanted_channel_layout) < 0)
    return -1;
  audio_hw_params->frame_size = av_samples_get_buffer_size(
      NULL, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
  audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(
      NULL, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq,
      audio_hw_params->fmt, 1);
  if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
    av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
    return -1;
  }
  return spec.size;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index) {
  AVFormatContext *ic = is->ic;
  AVCodecContext *avctx;
  const AVCodec *codec;
  AVDictionary *opts = NULL;
  const AVDictionaryEntry *t = NULL;
  int sample_rate;
  AVChannelLayout ch_layout = {0};
  int ret = 0;
  int stream_lowres = lowres;

  if (stream_index < 0 || stream_index >= ic->nb_streams) {
    return -1;
  }

  avctx = avcodec_alloc_context3(NULL);
  if (!avctx) {
    return AVERROR(ENOMEM);
  }

  ret =
      avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
  if (ret < 0) {
    goto fail;
  }
  avctx->pkt_timebase = ic->streams[stream_index]->time_base;

  codec = avcodec_find_decoder(avctx->codec_id);

  switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      is->last_audio_stream = stream_index;
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      is->last_subtitle_stream = stream_index;
      break;
    case AVMEDIA_TYPE_VIDEO:
      is->last_video_stream = stream_index;
      break;
  }

  avctx->codec_id = codec->id;
  // 优化处理，降低分辨率， 0 表示不降低分辨率，看是否需要
  if (stream_lowres > codec->max_lowres) {
    av_log(avctx, AV_LOG_WARNING,
           "The maximum value for lowres supported by the decoder is %d\n",
           codec->max_lowres);
    stream_lowres = codec->max_lowres;
  }
  avctx->lowres = stream_lowres;

  if (!av_dict_get(opts, "threads", NULL, 0))
    av_dict_set(&opts, "threads", "auto", 0);
  if (stream_lowres) {
    av_dict_set_int(&opts, "lowres", stream_lowres, 0);
  }
  if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
    goto fail;
  }
  if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
    av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
    ret = AVERROR_OPTION_NOT_FOUND;
    goto fail;
  }

  is->eof = 0;
  ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
  switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      sample_rate = avctx->sample_rate;
      ret = av_channel_layout_copy(&ch_layout, &avctx->ch_layout);
      if (ret < 0) {
        goto fail;
      }

      /* prepare audio output */
      if ((ret = audio_open(is, &ch_layout, sample_rate, &is->audio_tgt)) < 0)
        goto fail;
      is->audio_hw_buf_size = ret;
      is->audio_src = is->audio_tgt;
      is->audio_buf_size = 0;
      is->audio_buf_index = 0;

      /* init averaging filter */
      // exp(log(0.01) / AUDIO_DIFF_AVG_NB) 这个表达式的含义是:
      // 首先计算 log(0.01) 的结果是 -4.60517。
      // 然后将 -4.60517 除以 20，得到 -0.230258。
      // 最后计算 exp(-0.230258)，结果是 0.794328。
      is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
      is->audio_diff_avg_count = 0;
      /* since we do not have a precise anough audio FIFO fullness,
            we correct audio sync only if larger than this threshold */
      is->audio_diff_threshold =
          (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

      is->audio_stream = stream_index;
      is->audio_st = ic->streams[stream_index];

      if ((ret = decoder_init(&is->auddec, avctx, &is->audioq,
                              is->continue_read_thread)) < 0)
        goto fail;
      if (is->ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
        is->auddec.start_pts = is->audio_st->start_time;
        is->auddec.start_pts_tb = is->audio_st->time_base;
      }
      if ((ret = decoder_start(&is->auddec, audio_thread, "audio_decoder",
                               is)) < 0)
        goto out;
      SDL_PauseAudioDevice(audio_dev, 0);
      break;
    case AVMEDIA_TYPE_VIDEO:
      is->video_stream = stream_index;
      is->video_st = ic->streams[stream_index];

      if ((ret = decoder_init(&is->viddec, avctx, &is->videoq,
                              is->continue_read_thread)) < 0)
        goto fail;
      if ((ret = decoder_start(&is->viddec, video_thread, "video_decoder",
                               is)) < 0)
        goto out;
      //  is->queue_attachments_req = 1;
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      is->subtitle_stream = stream_index;
      is->subtitle_st = ic->streams[stream_index];

      if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq,
                              is->continue_read_thread)) < 0)
        goto fail;
      if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder",
                               is)) < 0)
        goto out;
      break;
    default:
      break;
  }
  goto out;

fail:
  avcodec_free_context(&avctx);
out:
  av_channel_layout_uninit(&ch_layout);
  av_dict_free(&opts);

  return ret;
}

static int decode_interrupt_cb(void *ctx) {
  VideoState *is = ctx;
  return is->abort_request;
}

// AV_DISPOSITION_ATTACHED_PIC 所表示的意思，可查看下方文档
// https://segmentfault.com/a/1190000018373504
static int stream_has_enough_packets(AVStream *st, int stream_id,
                                     PacketQueue *queue) {
  return stream_id < 0 || queue->abort_request ||
         (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
         queue->nb_packets > MIN_FRAMES &&
             (!queue->duration ||
              av_q2d(st->time_base) * queue->duration > 1.0);
}

static int is_realtime(AVFormatContext *s) {
  if (!strcmp(s->iformat->name, "rtp") || !strcmp(s->iformat->name, "rtsp") ||
      !strcmp(s->iformat->name, "sdp")) {
    return 1;
  }

  if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4))) {
    return 1;
  }
  return 0;
}

/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg) {
  VideoState *is = arg;
  AVFormatContext *ic = NULL;
  int err, i, ret;
  int st_index[AVMEDIA_TYPE_NB];
  AVPacket *pkt = NULL;
  int64_t stream_start_time;
  int pkt_in_play_range = 0;
  const AVDictionaryEntry *t;
  // int scan_all_pmts_set = 0;
  int64_t pkt_ts;

  SDL_mutex *wait_mutex = SDL_CreateMutex();
  if (!wait_mutex) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    ret = AVERROR(ENOMEM);
    goto fail;
  }

  memset(st_index, -1, sizeof(st_index));
  is->eof = 0;

  pkt = av_packet_alloc();
  if (!pkt) {
    av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
    ret = AVERROR(ENOMEM);
    goto fail;
  }
  ic = avformat_alloc_context();
  if (!ic) {
    av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
    ret = AVERROR(ENOMEM);
    goto fail;
  }
  // decode_interrupt_cb用于处理解码过程中的中断
  ic->interrupt_callback.callback = decode_interrupt_cb;
  ic->interrupt_callback.opaque = is;

  //检查 format_opts 字典中是否存在 "scan_all_pmts" 键值对。如果不存在,则添加一个值为 "1" 的 "scan_all_pmts" 键值对。
  //scan_all_pmts用于设置是否在打开输入流时扫描所有的 PMT(Program Map Table)。
  //当设置为 "1" 时,将扫描所有的 PMT,这可能会增加打开输入流的耗时,但可以确保捕获所有的流。
  //如果不设置这个选项,libavformat 会默认只扫描第一个找到的 PMT。
  // PMT(Program Map Table)
  // 如果不存在scan_all_pmts，这意味着没有明确指定是否扫描所有程序流元数据（PMTs，Program Map Tables），通常用于确定媒体流的正确编解码器。
  //if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
  //  // 告诉 FFmpeg 去扫描所有可用的 PMTs 以找到正确的编解码器，即使它们可能不是默认的。
  //  av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
  //  scan_all_pmts_set = 1;
  //}
  //// 尝试使用提供的输入格式 is->iformat 和选项 format_opts 打开媒体文件
  //err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
  err = avformat_open_input(&ic, is->filename, is->iformat, NULL);
  if (err < 0) {
    av_log(NULL, AV_LOG_ERROR, "%s could not open, err: %d\n", is->filename,
           err);
    ret = -1;
    goto fail;
  }

  //if (scan_all_pmts_set)
  //  av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
  //// 检查 format_opts 中是否有未识别的选项，并打印错误
  //if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
  //  av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
  //  ret = AVERROR_OPTION_NOT_FOUND;
  //  goto fail;
  //}
  is->ic = ic;

  /*
    当设置了这个标志时,FFmpeg 会尝试根据输入数据流的特性,生成一个连续和合理的 PTS 值,即使原始数据流中的 PTS 值不连续或缺失。
    这对于一些有问题的输入流很有用,比如 PTS 值缺失或不连续的情况。FFmpeg 会通过算法推算出合理的 PTS 值,以保证视频和音频的正确播放。
    如果没有设置这个标志,FFmpeg 会严格使用输入数据流中提供的 PTS 值,即使它们不连续或缺失。这可能导致视频和音频播放出现问题。
   */
  //  if (genpts)
  //      ic->flags |= AVFMT_FLAG_GENPTS;

  // av_format_inject_global_side_data(ic) 的作用就是将这些全局侧边数据复制到每个流的侧边数据中,这样可以确保在后续的解码和渲染过程中,这些附加信息能够被正确地传递和使用。
  /*av_format_inject_global_side_data(ic);*/

  //AVDictionary **opts = setup_find_stream_info_opts(ic, codec_opts);
  //int orig_nb_streams = ic->nb_streams;

  //err = avformat_find_stream_info(ic, opts);
  err = avformat_find_stream_info(ic, NULL);

  //for (i = 0; i < orig_nb_streams; i++) av_dict_free(&opts[i]);
  //av_freep(&opts);

  if (err < 0) {
    av_log(NULL, AV_LOG_WARNING, "%s: could not find codec parameters\n",
           is->filename);
    ret = -1;
    goto fail;
  }

  if (ic->pb) {
    // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
    ic->pb->eof_reached = 0;
  }

  //当输入格式支持基于字节偏移量的seek,且时间戳信息不连续,如果时间戳不连续,并且不是 Ogg 格式时,seek_by_bytes 变量会被设置为 true
  if (seek_by_bytes < 0)
    seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                    !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                    strcmp("ogg", ic->iformat->name);

  is->max_frame_duration =
      (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

  // 与 sprintf() 相比, av_asprintf() 的优势在于它能够自动分配足够的内存空间, 避免了缓冲区溢出的风险。同时, 返回的是一个动态分配的字符串指针, 使用更加灵活。
  // 动态内存分配和字符串构建函数，在格式化字符串的同时，分配内存，返回字符串指针
  if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
    window_title = av_asprintf("%s - %s", t->value, input_filename);

  /* if seeking requested, we execute it */
  if (start_time != AV_NOPTS_VALUE) {
    int64_t timestamp;

    timestamp = start_time;
    /* add the stream start time */
    if (ic->start_time != AV_NOPTS_VALUE) {
      timestamp += ic->start_time;
    }
    ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
    if (ret < 0) {
      av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
             is->filename, (double)timestamp / AV_TIME_BASE);
    }
  }

  is->realtime = is_realtime(ic);

  if (show_status) {
    av_dump_format(ic, 0, is->filename, 0);
  }
  // 获取流索引之前，先进行遍历，看播放前有没有选择想要播放对应索引的数据流，没有选择的话，wanted_stream_spec[type]为false，即无效
  // 如果没有设置选择想要播放的流索引的话，这块可以去掉处理，抄写时说明下把它注释掉
  for (i = 0; i < ic->nb_streams; i++) {
    AVStream *st = ic->streams[i];
    enum AVMediaType type = st->codecpar->codec_type;
    st->discard = AVDISCARD_ALL;
    if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
      if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
        st_index[type] = i;
  }
  for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
    if (wanted_stream_spec[i] && st_index[i] == -1) {
      av_log(NULL, AV_LOG_ERROR,
             "Stream specifier %s does not match any %s stream\n",
             wanted_stream_spec[i], av_get_media_type_string(i));
      st_index[i] = INT_MAX;
    }
  }

  if (!video_disable)
    st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(
        ic, AVMEDIA_TYPE_VIDEO, st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
  if (!audio_disable)
    st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(
        ic, AVMEDIA_TYPE_AUDIO, st_index[AVMEDIA_TYPE_AUDIO],
        st_index[AVMEDIA_TYPE_VIDEO], NULL, 0);
  if (!video_disable && !subtitle_disable)
    st_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(
        ic, AVMEDIA_TYPE_SUBTITLE, st_index[AVMEDIA_TYPE_SUBTITLE],
        (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO]
                                           : st_index[AVMEDIA_TYPE_VIDEO]),
        NULL, 0);

  is->show_mode = show_mode;
  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
    AVCodecParameters *codecpar = st->codecpar;
    AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
    if (codecpar->width)
      set_default_window_size(codecpar->width, codecpar->height, sar);
  }

  /* open the streams */
  if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
    stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
  }

  ret = -1;
  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
  }

  if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
    stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
  }

  if (is->video_stream < 0 && is->audio_stream < 0) {
    av_log(NULL, AV_LOG_FATAL,
           "Failed to open file '%s' or configure filtergraph\n", is->filename);
    ret = -1;
    goto fail;
  }

  if (infinite_buffer < 0 && is->realtime) {
    infinite_buffer = 1;
  }

  // 暂停和恢复媒体流读取的完整功能。av_read_pause 用于在用户暂停视频播放时停止读取数据,
  // 而 av_read_play 用于在用户恢复播放时重新开始读取和解码数据。用于流媒体，网络流(e.g. RTSP stream)
  for (;;) {
    if (is->abort_request) break;
    if (is->paused != is->last_paused) {
      is->last_paused = is->paused;
      if (is->paused)
        is->read_pause_return = av_read_pause(ic);
      else
        av_read_play(ic);
    }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
    if (is->paused && (!strcmp(ic->iformat->name, "rtsp") ||
                       (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
      /* wait 10 ms to avoid trying to get another packet */
      /* XXX: horrible */
      SDL_Delay(10);
      continue;
    }
#endif
    if (is->seek_req) {
      int64_t seek_target = is->seek_pos;
      int64_t seek_min =
          is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
      int64_t seek_max =
          is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
      // FIXME the +-2 is due to rounding being not done in the correct direction in generation
      //      of the seek_pos/seek_rel variables

      ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max,
                               is->seek_flags);
      if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", is->ic->url);
      } else {
        if (is->audio_stream >= 0) packet_queue_flush(&is->audioq);
        if (is->subtitle_stream >= 0) packet_queue_flush(&is->subtitleq);
        if (is->video_stream >= 0) packet_queue_flush(&is->videoq);
        if (is->seek_flags & AVSEEK_FLAG_BYTE) {
          set_clock(&is->extclk, NAN, 0);
        } else {
          set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
        }
      }
      is->seek_req = 0;
      is->queue_attachments_req = 1;
      is->eof = 0;
      if (is->paused) {
        step_to_next_frame(is);
      }
    }
    // 处理视频流附件数据
    if (is->queue_attachments_req) {
      // 检查当前视频流是否包含了附件图像
      if (is->video_st &&
          is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
        if ((ret = av_packet_ref(pkt, &is->video_st->attached_pic)) < 0)
          goto fail;
        packet_queue_put(&is->videoq, pkt);
        // 加一个空 AVPacket 到视频队列中,用于标记附件数据的结束
        packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
      }
      is->queue_attachments_req = 0;
    }

    /* if the queue are full, no need to read more */
    /* 限制从流获取packet数据，实时流的话就不限制 */
    if (infinite_buffer < 1 &&
        (is->audioq.size + is->videoq.size + is->subtitleq.size >
             MAX_QUEUE_SIZE ||
         (stream_has_enough_packets(is->audio_st, is->audio_stream,
                                    &is->audioq) &&
          stream_has_enough_packets(is->video_st, is->video_stream,
                                    &is->videoq) &&
          stream_has_enough_packets(is->subtitle_st, is->subtitle_stream,
                                    &is->subtitleq)))) {
      /* wait 10 ms */
      SDL_LockMutex(wait_mutex);
      SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
      SDL_UnlockMutex(wait_mutex);
      continue;
    }
    // 是否循环播放
    if (!is->paused &&
        (!is->audio_st || (is->auddec.finished == is->audioq.serial &&
                           frame_queue_nb_remaining(&is->sampq) == 0)) &&
        (!is->video_st || (is->viddec.finished == is->videoq.serial &&
                           frame_queue_nb_remaining(&is->pictq) == 0))) {
      if (loop != 1 && (!loop || --loop)) {
        stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
      } else if (autoexit) {
        ret = AVERROR_EOF;
        goto fail;
      }
    }
    ret = av_read_frame(ic, pkt);
    if (ret < 0) {
      // 检查输入流是否到达文件末尾，到文件末尾，推入空包
      if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
        if (is->video_stream >= 0)
          packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
        if (is->audio_stream >= 0)
          packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
        if (is->subtitle_stream >= 0)
          packet_queue_put_nullpacket(&is->subtitleq, pkt, is->subtitle_stream);
        is->eof = 1;
      }
      if (ic->pb && ic->pb->error) {
        if (autoexit)
          goto fail;
        else
          break;
      }
      SDL_LockMutex(wait_mutex);
      SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
      SDL_UnlockMutex(wait_mutex);
      continue;
    } else {
      is->eof = 0;
    }
    /* check if packet is in play range specified by user, then queue, otherwise discard */
    stream_start_time = ic->streams[pkt->stream_index]->start_time;
    pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
    /*
          按照给定的想要播放的时长进行获取packet数据，不在这个范围内的数据都丢掉处理，若没有给定播放时长范围，就从0开始播放
          这里的duration是指想要播放的范围时长
          pkt_in_play_range用于确定一个数据包是否在播放范围内的条件判断
          如果一个数据包不在播放范围内, 就会丢弃该数据包,不进行解码和渲染。
          这样可以避免解码和渲染超出播放范围的数据
          pkt_in_play_range表示当前数据包是否在播放范围内

          duration == AV_NOPTS_VALUE时，它会被认为是在播放范围内的，即用户没有设置播放范围duration
           1. 数据包的 duration 信息是可选的。并不是所有的媒体容器和编码格式都会提供明确的 duration 信息。
           2. 如果一个数据包没有 duration 信息,通常意味着:
           这个数据包是一个关键帧或者 I 帧
           它标记了媒体流的一个重要时间点
           即使无法确定它的持续时间,也应该确保这个关键帧被正确地解码和渲染
           3. 如果直接将没有 duration 信息的数据包排除在播放范围之外,可能会导致:
           丢失重要的关键帧
           造成播放中断或者画面跳跃
         */
    pkt_in_play_range =
        duration == AV_NOPTS_VALUE ||
        (pkt_ts -
         (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                    av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) /
                    1000000 <=
            ((double)duration / 1000000);
    if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
      packet_queue_put(&is->audioq, pkt);
    } else if (pkt->stream_index == is->video_stream && pkt_in_play_range &&
               !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      packet_queue_put(&is->videoq, pkt);
    } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
      packet_queue_put(&is->subtitleq, pkt);
    } else {
      av_packet_unref(pkt);
    }
  }

  ret = 0;
fail:
  if (ic && !is->ic) {
    avformat_close_input(&ic);
  }

  av_packet_free(&pkt);
  if (ret != 0) {
    SDL_Event event;

    event.type = FF_QUIT_EVENT;
    event.user.data1 = is;
    SDL_PushEvent(&event);
  }
  SDL_DestroyMutex(wait_mutex);
  return 0;
}

static VideoState *stream_open(const char *filename,
                               const AVInputFormat *iformat) {
  VideoState *is;

  is = av_mallocz(sizeof(VideoState));
  if (!is) {
    return NULL;
  }
  is->last_video_stream = is->video_stream = -1;
  is->last_audio_stream = is->audio_stream = -1;
  is->last_subtitle_stream = is->subtitle_stream = -1;
  is->filename = av_strdup(filename);
  if (!is->filename) {
    stream_close(is);
    return NULL;
  }
  is->iformat = iformat;
  is->ytop = 0;
  is->xleft = 0;

  /* start video display */
  if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) <
          0 ||
      frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) <
          0 ||
      frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0) {
    stream_close(is);
    return NULL;
  }

  if (packet_queue_init(&is->videoq) < 0 ||
      packet_queue_init(&is->audioq) < 0 ||
      packet_queue_init(&is->subtitleq) < 0) {
    stream_close(is);
    return NULL;
  }

  if (!(is->continue_read_thread = SDL_CreateCond())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    stream_close(is);
    return NULL;
  }

  init_clock(&is->vidclk, &is->videoq.serial);
  init_clock(&is->audclk, &is->audioq.serial);
  init_clock(&is->extclk, &is->extclk.serial);
  is->audio_clock_serial = -1;

  // 设置起始播放的音量
  if (startup_volume < 0)
    av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n",
           startup_volume);
  if (startup_volume > 100)
    av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n",
           startup_volume);
  startup_volume = av_clip(startup_volume, 0, 100);
  startup_volume =
      av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
  is->audio_volume = startup_volume;
  is->muted = 0;
  is->av_sync_type = av_sync_type;
  is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
  if (!is->read_tid) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
    stream_close(is);
    return NULL;
  }
  return is;
}

static void toggle_full_screen(VideoState *is) {
  is_full_screen = !is_full_screen;
  SDL_SetWindowFullscreen(window,
                          is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

// 用于处理视频刷新循环中的事件等待，负责控制视频帧的显示时机,确保视频与音频的同步,并处理各种用户输入和系统事件
static void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
  double remaining_time = 0.0;
  // SDL_PumpEvents作用是获取并处理系统中发生的所有事件。
  SDL_PumpEvents();
  // SDL_PeepEvents 用于从 SDL 的内部事件队列中获取、查看或删除事件
  // 成功读取到事件,循环会退出
  while (
      !SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
    if (!cursor_hidden &&
        av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
      SDL_ShowCursor(0);
      cursor_hidden = 1;
    }
    if (remaining_time > 0.0) {
      av_usleep((int64_t)(remaining_time * 1000000.0));
    }
    remaining_time = REFRESH_RATE;
    if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
      video_refresh(is, &remaining_time);
    SDL_PumpEvents();
  }
}

/* handle an event sent by the GUI */
static void event_loop(VideoState *cur_stream) {
  SDL_Event event;
  double incr, pos, frac;

  for (;;) {
    double x;
    refresh_loop_wait_event(cur_stream, &event);
    switch (event.type) {
      case SDL_KEYDOWN:
        if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE ||
            event.key.keysym.sym == SDLK_q) {
          do_exit(cur_stream);
          break;
        }
        // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
        if (!cur_stream->width) {
          continue;
        }
        switch (event.key.keysym.sym) {
          case SDLK_f:
            toggle_full_screen(cur_stream);
            cur_stream->force_refresh = 1;
            break;
          case SDLK_SPACE:
            toggle_pause(cur_stream);
            break;
          case SDLK_m:
            toggle_mute(cur_stream);
            break;
          case SDLK_0:
            update_volume(cur_stream, 1, SDL_VOLUME_STEP);
            break;
          case SDLK_9:
            update_volume(cur_stream, -1, SDL_VOLUME_STEP);
            break;
          case SDLK_s:  // S: Step to next frame
            step_to_next_frame(cur_stream);
            break;
          case SDLK_LEFT:
            incr = -10.0;
            goto do_seek;
          case SDLK_RIGHT:
            incr = 10.0;
            goto do_seek;
          case SDLK_UP:
            incr = 60.0;
            goto do_seek;
          case SDLK_DOWN:
            incr = -60.0;
          do_seek:
            if (seek_by_bytes) {
              pos = -1;
              if (pos < 0 && cur_stream->video_stream >= 0)
                pos = frame_queue_last_pos(&cur_stream->pictq);
              if (pos < 0 && cur_stream->audio_stream >= 0)
                pos = frame_queue_last_pos(&cur_stream->sampq);
              if (pos < 0) {
                pos = avio_tell(cur_stream->ic->pb);
              }
              if (cur_stream->ic->bit_rate)
                incr *= cur_stream->ic->bit_rate / 8.0;
              else
                incr *= 180000.0;
              pos += incr;
              stream_seek(cur_stream, pos, incr, 1);
            } else {
              pos = get_master_clock(cur_stream);
              if (isnan(pos)) {
                pos = (double)cur_stream->seek_pos / AV_TIME_BASE;
              }
              pos += incr;
              if (cur_stream->ic->start_time != AV_NOPTS_VALUE &&
                  pos < cur_stream->ic->start_time / (double)AV_TIME_BASE) {
                pos = cur_stream->ic->start_time / (double)AV_TIME_BASE;
              }
              stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE),
                          (int64_t)(incr * AV_TIME_BASE), 0);
            }
            break;
          default:
            break;
        }
        break;
      case SDL_MOUSEBUTTONDOWN:
        if (exit_on_mousedown) {
          do_exit(cur_stream);
          break;
        }
        if (event.button.button == SDL_BUTTON_LEFT) {
          // 双击全屏操作
          static int64_t last_mouse_left_click = 0;
          if (av_gettime_relative() - last_mouse_left_click <= 500000) {
            toggle_full_screen(cur_stream);
            cur_stream->force_refresh = 1;
            last_mouse_left_click = 0;
          } else {
            last_mouse_left_click = av_gettime_relative();
          }
        }
      case SDL_MOUSEMOTION:
        if (cursor_hidden) {
          SDL_ShowCursor(1);
          cursor_hidden = 0;
        }
        cursor_last_shown = av_gettime_relative();
        // 鼠标右键拖动seek
        if (event.type == SDL_MOUSEBUTTONDOWN) {
          if (event.button.button != SDL_BUTTON_RIGHT) break;
          x = event.button.x;
        } else {
          if (!(event.motion.state & SDL_BUTTON_RMASK)) break;
          x = event.motion.x;
        }
        if (seek_by_bytes || cur_stream->ic->duration <= 0) {
          uint64_t size = avio_size(cur_stream->ic->pb);
          stream_seek(cur_stream, size * x / cur_stream->width, 0, 1);
        } else {
          int64_t ts;
          int ns, hh, mm, ss;
          int tns, thh, tmm, tss;
          tns = cur_stream->ic->duration / 1000000LL;
          thh = tns / 3600;
          tmm = (tns % 3600) / 60;
          tss = (tns % 60);
          frac = x / cur_stream->width;
          ns = frac * tns;
          hh = ns / 3600;
          mm = (ns % 3600) / 60;
          ss = (ns % 60);
          av_log(NULL, AV_LOG_INFO,
                 "Seek to %2.0f%% (%2d:%02d:%02d) of total duration "
                 "(%2d:%02d:%02d)       \n",
                 frac * 100, hh, mm, ss, thh, tmm, tss);
          ts = frac * cur_stream->ic->duration;
          if (cur_stream->ic->start_time != AV_NOPTS_VALUE)
            ts += cur_stream->ic->start_time;
          stream_seek(cur_stream, ts, 0, 0);
        }
        break;
      case SDL_WINDOWEVENT:
        switch (event.window.event) {
          case SDL_WINDOWEVENT_SIZE_CHANGED:
            screen_width = cur_stream->width = event.window.data1;
            screen_height = cur_stream->height = event.window.data2;
          case SDL_WINDOWEVENT_EXPOSED:
            // 当窗口状态发生变化,比如从最小化状态恢复或从遮挡状态露出时,会触发 SDL_WINDOWEVENT_EXPOSED 事件。这可用于重新渲染视频画面。
            cur_stream->force_refresh = 1;
        }
        break;
      case SDL_QUIT:
      case FF_QUIT_EVENT:
        do_exit(cur_stream);
        break;
      default:
        break;
    }
  }
}

/* ============= Playback option settings, set by user ============= */
static void set_width(double width) {
  if (width > 0) screen_width = width;
}

static void set_height(double height) {
  if (height > 0) screen_height = height;
}

static void set_full_screen(int full_screen) { is_full_screen = full_screen; }

static int set_format(const char *input_name) {
  file_iformat = av_find_input_format(input_name);
  if (!file_iformat) {
    av_log(NULL, AV_LOG_FATAL, "Unknown input format: %s\n", input_name);
    return AVERROR(EINVAL);
  }
  return 0;
}

static int set_sync_type(const char *arg) {
  if (!strcmp(arg, "audio"))
    av_sync_type = AV_SYNC_AUDIO_MASTER;
  else if (!strcmp(arg, "video"))
    av_sync_type = AV_SYNC_VIDEO_MASTER;
  else if (!strcmp(arg, "ext"))
    av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
  else {
    av_log(NULL, AV_LOG_ERROR, "Unknown value for: %s\n", arg);
    exit(1);
  }
  return 0;
}

static int set_show_mode(enum ShowMode mode) {
  // can't support wave and rdtf mode
  if (show_mode == SHOW_MODE_NONE) {
    if (show_mode > 0 && show_mode < SHOW_MODE_NB - 1) {
      show_mode = mode;
    }
  }
  return 0;
}

static void set_play_start_time(double play_start_time) {
  if (play_start_time > 0) {
    start_time = play_start_time;
  } else {
    start_time = 0;
  }
}

static void set_window_always_on_top(int always_on_top) {
  alwaysontop = always_on_top;
}

static void set_loop_count(int loop_count) { loop = loop_count; }

static void set_disable_to_show_video(int no_display) {
  display_disable = no_display;
}

static void set_audio_disable(int disable_audio) {
  audio_disable = disable_audio;
}

static void set_video_disable(int disable_video) {
  video_disable = disable_video;
}

static void set_disable_subtitling(int disable_subtitling) {
  subtitle_disable = disable_subtitling;
}

// 这个duration在read_thread里面使用到
static void set_play_duration(double play_duration) {
  duration = play_duration;
}

void set_is_need_seek_by_bytes(int is_need_seek_by_bytes) {
  seek_by_bytes = is_need_seek_by_bytes;
}

static void set_window_bordless(int noborder) { borderless = noborder; }

static void set_start_volume(int volume) { startup_volume = volume; }

static void set_auto_exit_at_end(int auto_exit) { autoexit = auto_exit; }

static void set_exit_on_keydown(int keydown) { exit_on_keydown = keydown; }

static void set_exit_on_mousedown(int mousedown) {
  exit_on_mousedown = mousedown;
}
/* ============= Playback option settings, set by user ============= */

/*
循环切换 : 针对有多个音频流以及视频流 , 如电视节目 TS 流 , 多个电视台信号在一个流中 , 可以通过切换 音频流 / 视频流 / 节目 等选择不同的电视台信号进行观看 ;
循环切换音频流 : A ; ( Audio )
循环切换视频流 : V ; ( Vedio )
循环切换字幕流 : T ;
循环切换节目 : C ;
循环切换过滤器或显示模式 : W ;
*/
static void show_usage(void) {
  av_log(NULL, AV_LOG_INFO, "Simple media player\n");
  av_log(NULL, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
  av_log(NULL, AV_LOG_INFO, "\n");
  printf(
      "\nWhile playing:\n"
      "q, ESC              quit\n"
      "f                   toggle full screen\n"
      "SPC                 pause\n"
      "m                   toggle mute\n"
      "9, 0                decrease and increase volume respectively\n"
      "/, *                decrease and increase volume respectively\n"
      "a                   cycle audio channel in the current program\n"
      "v                   cycle video channel\n"
      "t                   cycle subtitle channel in the current program\n"
      "c                   cycle program\n"
      "w                   cycle video filters or show modes\n"
      "s                   activate frame-step mode\n"
      "left/right          seek backward/forward 10 seconds or to custom "
      "interval if -seek_interval is set\n"
      "down/up             seek backward/forward 1 minute\n"
      "page down/page up   seek backward/forward 10 minutes\n"
      "right mouse click   seek to percentage in file corresponding to "
      "fraction of width\n"
      "left double-click   toggle full screen\n");
}

/* Called from the main */
int main(int argc, char **argv) {
  VideoState *is;

  /* register all codecs, demux and protocols */
  //avdevice_register_all();
  avformat_network_init();

  /*
设置了对 SIGINT 和 SIGTERM 信号的处理函数。
SIGINT 信号通常由用户输入 Ctrl+C 触发,表示进程被用户中断。
SIGTERM 信号通常由操作系统发送,表示进程被正常终止。
sigterm_handler 是一个自定义的信号处理函数,当接收到相应的信号时,就会执行这个函数。
通过设置信号处理函数,可以让进程在收到终止信号时执行一些清理操作,比如释放资源、保存状态等,而不是直接退出。
*/
  signal(SIGINT, sigterm_handler);  /* Interrupt (ANSI).    */
  signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

  input_filename = argv[1];
  if (!input_filename) {
    show_usage();
    exit(1);
  }

  if (display_disable) {
    video_disable = 1;
  }
  int flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
  if (audio_disable) {
    flags &= ~SDL_INIT_AUDIO;
  }

  if (display_disable) {
    flags &= ~SDL_INIT_VIDEO;
  }
  if (SDL_Init(flags)) {
    av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n",
           SDL_GetError());
    av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
    exit(1);
  }

  // 用于控制特定类型事件的状态.
  // SDL_IGNORE将事件自动从事件队列中删除，并且不会被过滤。优化性能或者过滤掉不需要的事件
  SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
  SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

  if (!display_disable) {
    int flags = SDL_WINDOW_HIDDEN;
    if (alwaysontop) {
      flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    if (borderless)
      flags |= SDL_WINDOW_BORDERLESS;
    else
      flags |= SDL_WINDOW_RESIZABLE;

    window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, default_width,
                              default_height, flags);
    // 用于设置 SDL 内部的环境变量或配置选项.通过使用 SDL_SetHint,可以更好地控制 SDL 的行为,针对不同的应用场景进行个性化的配置。
    /* 一些常见的 SDL 内置配置选项包括:
          SDL_HINT_RENDER_SCALE_QUALITY: 设置渲染质量,可选值为 "nearest", "linear", "best"。
          SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS: 是否允许在应用程序处于后台时接收手柄事件。
          SDL_HINT_VIDEO_HIGHDPI_DISABLED: 是否禁用高 DPI 支持。
          SDL_HINT_GRAB_KEYBOARD: 是否抓取键盘输入。*/
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (window) {
      renderer = SDL_CreateRenderer(
          window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
      if (!renderer) {
        av_log(NULL, AV_LOG_WARNING,
               "Failed to initialize a hardware accelerated renderer: %s\n",
               SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, 0);
      }
      if (renderer) {
        if (!SDL_GetRendererInfo(renderer, &renderer_info))
          av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n",
                 renderer_info.name);
      }
    }
    if (!window || !renderer || !renderer_info.num_texture_formats) {
      av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s",
             SDL_GetError());
      do_exit(NULL);
    }
  }

  is = stream_open(input_filename, file_iformat);
  if (!is) {
    av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
    do_exit(NULL);
  }

  event_loop(is);

  /* never returns */

  return 0;
}

//  static const OptionDef options[] = {
//      CMDUTILS_COMMON_OPTIONS
//      { "x",                  OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_width }, "force displayed width", "width" },
//      { "y",                  OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_height }, "force displayed height", "height" },
//      { "fs",                 OPT_TYPE_BOOL,            0, { &is_full_screen }, "force full screen" },
//      { "an",                 OPT_TYPE_BOOL,            0, { &audio_disable }, "disable audio" },  //禁用音频
//      { "vn",                 OPT_TYPE_BOOL,            0, { &video_disable }, "disable video" },  //禁用视频
//      { "sn",                 OPT_TYPE_BOOL,            0, { &subtitle_disable }, "disable subtitling" },  //禁用字幕
//      { "ast",                OPT_TYPE_STRING, OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_AUDIO] }, "select desired audio stream", "stream_specifier" },  // 选择想要的音频流  -ast index 播放流索引为index的音频流
//      { "vst",                OPT_TYPE_STRING, OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_VIDEO] }, "select desired video stream", "stream_specifier" },  // 选择想要的视频流  -vst index 播放流索引为index的视频流
//      { "sst",                OPT_TYPE_STRING, OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE] }, "select desired subtitle stream", "stream_specifier" },  // 选择想要的字幕流  -sst index 播放流索引为index的字幕流
//      { "ss",                 OPT_TYPE_TIME,            0, { &start_time }, "seek to a given position in seconds", "pos" },  // 设置播放开始时间
//      { "t",                  OPT_TYPE_TIME,            0, { &duration }, "play  \"duration\" seconds of audio/video", "duration" },  // 设置播放持续时间
//      { "bytes",              OPT_TYPE_INT,             0, { &seek_by_bytes }, "seek by bytes 0=off 1=on -1=auto", "val" },  // 按字节而不是按时间进行seek
//      { "seek_interval",      OPT_TYPE_FLOAT,           0, { &seek_interval }, "set seek interval for left/right keys, in seconds", "seconds" },  //设置左/右键seek的时间间隔
//      { "nodisp",             OPT_TYPE_BOOL,            0, { &display_disable }, "disable graphical display" },  // 是否显示视图
//      { "noborder",           OPT_TYPE_BOOL,            0, { &borderless }, "borderless window" },  // 是否有边框
//      { "alwaysontop",        OPT_TYPE_BOOL,            0, { &alwaysontop }, "window always on top" },  // 窗口是否总在最上层
//      { "volume",             OPT_TYPE_INT,             0, { &startup_volume}, "set startup volume 0=min 100=max", "volume" },  // 设置初始音量
//      { "f",                  OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_format }, "force format", "fmt" },  // 特定的媒体格式
//      { "stats",              OPT_TYPE_BOOL,   OPT_EXPERT, { &show_status }, "show status", "" },  // 显示状态信息，这个可以先不用管
//      { "fast",               OPT_TYPE_BOOL,   OPT_EXPERT, { &fast }, "non spec compliant optimizations", "" },  // 专家级选项，用于控制解码行为
//      { "genpts",             OPT_TYPE_BOOL,   OPT_EXPERT, { &genpts }, "generate pts", "" },  // 专家级选项，用于控制解码行为
//      { "drp",                OPT_TYPE_INT,    OPT_EXPERT, { &decoder_reorder_pts }, "let decoder reorder pts 0=off 1=on -1=auto", ""},  // 专家级选项，用于控制解码行为
//      { "lowres",             OPT_TYPE_INT,    OPT_EXPERT, { &lowres }, "", "" },       // 控制视频解码输出的分辨率
//      { "sync",               OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT, { .func_arg = opt_sync }, "set audio-video sync. type (type=audio/video/ext)", "type" },  //设置音视频同步方式
//      // 控制视频结束后是否自动退出
//      { "autoexit",           OPT_TYPE_BOOL,   OPT_EXPERT, { &autoexit }, "exit at the end", "" },
//      { "exitonkeydown",      OPT_TYPE_BOOL,   OPT_EXPERT, { &exit_on_keydown }, "exit on key down", "" },
//      { "exitonmousedown",    OPT_TYPE_BOOL,   OPT_EXPERT, { &exit_on_mousedown }, "exit on mouse down", "" },
//      // 设置循环播放次数
//      { "loop",               OPT_TYPE_INT,    OPT_EXPERT, { &loop }, "set number of times the playback shall be looped", "loop count" },
//      // 在CUP性能不足时丢弃帧，以及控制输入缓冲区大小
//      { "framedrop",          OPT_TYPE_BOOL,   OPT_EXPERT, { &framedrop }, "drop frames when cpu is too slow", "" },
//      { "infbuf",             OPT_TYPE_BOOL,   OPT_EXPERT, { &infinite_buffer }, "don't limit the input buffer size (useful with realtime streams)", "" },
//      // 设置窗口标题和位置
//      { "window_title",       OPT_TYPE_STRING,          0, { &window_title }, "set window title", "window title" },
//      { "left",               OPT_TYPE_INT,    OPT_EXPERT, { &screen_left }, "set the x position for the left of the window", "x pos" },
//      { "top",                OPT_TYPE_INT,    OPT_EXPERT, { &screen_top }, "set the y position for the top of the window", "y pos" },
//      { "vf",                 OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT, { .func_arg = opt_add_vfilter }, "set video filters", "filter_graph" },   //设置视频滤镜
//      { "af",                 OPT_TYPE_STRING,          0, { &afilters }, "set audio filters", "filter_graph" },  // 设置音频滤镜
//      // 设置实时傅立叶变化的相关参数
//      { "rdftspeed",          OPT_TYPE_INT, OPT_AUDIO | OPT_EXPERT, { &rdftspeed }, "rdft speed", "msecs" },
//      { "showmode",           OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_show_mode}, "select show mode (0 = video, 1 = waves, 2 = RDFT)", "mode" },
//      { "i",                  OPT_TYPE_BOOL,            0, { &dummy}, "read specified file", "input_file"},  // 指定输入文件
//      { "codec",              OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_codec}, "force decoder", "decoder_name" },  //指定解码器
//      // 指定相关解码器
//      { "acodec",             OPT_TYPE_STRING, OPT_EXPERT, {    &audio_codec_name }, "force audio decoder",    "decoder_name" },
//      { "scodec",             OPT_TYPE_STRING, OPT_EXPERT, { &subtitle_codec_name }, "force subtitle decoder", "decoder_name" },
//      { "vcodec",             OPT_TYPE_STRING, OPT_EXPERT, {    &video_codec_name }, "force video decoder",    "decoder_name" },
//      { "autorotate",         OPT_TYPE_BOOL,            0, { &autorotate }, "automatically rotate video", "" },  // 自动旋转视频
//      { "find_stream_info",   OPT_TYPE_BOOL, OPT_INPUT | OPT_EXPERT, { &find_stream_info },  // 读取并解码流以填充缺失的信息
//          "read and decode the streams to fill missing information with heuristics" },
//      { "filter_threads",     OPT_TYPE_INT,    OPT_EXPERT, { &filter_nbthreads }, "number of filter threads per graph" },  // 设置每个图像滤镜的线程数
//      { NULL, },
//  };