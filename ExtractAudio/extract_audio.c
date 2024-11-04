#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <libavutil/log.h>
#include <libavformat/avio.h>
#include <libavformat/avformat.h>

#define ADTS_HEADER_LEN 7; //7个字节

// aac每帧开头都要填写对应的格式信息
void adts_header(char* szAdtsHeader, int dataLen) {
	// aac级别，0: AAC Main 1:AAC LC (Low Complexity) 2:AAC SSR (Scalable Sample Rate) 3:AAC LTP (Long Term Prediction)
	int audio_object_type = 2;
	// 采样率下标，下标4表示采样率为44100
	int sampling_frequency_index = 4;
	// 声道数
	int channel_config = 2;

	/* 这里说明一下，上面这三个参数目前是写定的状态，这三个数据是根据提取的音频信息进行设置的，
	遇到不同采样率的音频数据就需要做相应的修改，其实这三个参数可以根据不同的音频源以传入的形式进行处理 */
	//当找到音频流索引后，通过下面方式进行获取
	//int audio_object_type = fmt_ctx->streams[audio_stream_index]->codecpar->profile;
	//int channel_config = fmt_ctx->streams[audio_stream_index]->codecpar->channels;
	//int sampling_frequency_index = fmt_ctx->streams[audio_stream_index]->codecpar->sample_rate;

	// ADTS帧长度,包括ADTS长度和AAC声音数据长度的和。
	int adtsLen = dataLen + 7;

	szAdtsHeader[0] = 0xff;         //syncword:0xfff 代表一个ADTS帧的开始,用于同步   高8bits
	szAdtsHeader[1] = 0xf0;         //syncword:0xfff 因为它的存在,可以在任意帧解码   低4bits
	szAdtsHeader[1] |= (0 << 3);    //ID:MPEG Version:0 for MPEG-4,1 for MPEG-2    1bit
	szAdtsHeader[1] |= (0 << 1);    //Layer:0                                      2bits 
	szAdtsHeader[1] |= 1;           //protection absent:1                          1bit

	szAdtsHeader[2] = (audio_object_type - 1) << 6;            //profile:audio_object_type - 1  表示使用哪个级别的AAC             2bits
	szAdtsHeader[2] |= (sampling_frequency_index & 0x0f) << 2; //sampling frequency index(采样率的下标):sampling_frequency_index  4bits 
	szAdtsHeader[2] |= (0 << 1);                             //private bit:0                                                   1bit
	szAdtsHeader[2] |= (channel_config & 0x04) >> 2;           //channel configuration(声道数):channel_config                    高1bit
	
	szAdtsHeader[3] = (channel_config & 0x03) << 6;     //channel configuration:channel_config      低2bits
	szAdtsHeader[3] |= (0 << 5);                      //original：0                               1bit
	szAdtsHeader[3] |= (0 << 4);                      //home：0                                   1bit
	szAdtsHeader[3] |= (0 << 3);                      //copyright id bit：0                       1bit  
	szAdtsHeader[3] |= (0 << 2);                      //copyright id start：0                     1bit
	szAdtsHeader[3] |= ((adtsLen & 0x1800) >> 11);           //frame length：value   高2bits

	szAdtsHeader[4] = (uint8_t)((adtsLen & 0x7f8) >> 3);     //frame length:value    中间8bits
	szAdtsHeader[5] = (uint8_t)((adtsLen & 0x7) << 5);       //frame length:value    低3bits
	szAdtsHeader[5] |= 0x1f;                                 //buffer fullness:0x7ff 高5bits
	szAdtsHeader[6] = 0xfc;                                  //number_of_raw_data_blocks_in_frame
}

int main(int argc, char* argv[])
{
	int err_code;
	char errors[1024];
	char* src_filename = NULL;
	char* dst_filename = NULL;

	av_log_set_level(AV_LOG_DEBUG);

	if (argc < 3) {
		av_log(NULL, AV_LOG_DEBUG, "the count of parameters should be more than three!\n");
		return -1;
	}

	src_filename = argv[1];
	dst_filename = argv[2];
	if (src_filename == NULL || dst_filename == NULL) {
		av_log(NULL, AV_LOG_DEBUG, "src or dts file is null, please check them!\n");
		return -1;
	}

	//av_register_all();

	FILE* dst_fd = NULL;
	dst_fd = fopen(dst_filename, "wb");
	if (!dst_fd) {
		av_log(NULL, AV_LOG_DEBUG, "Could not open destination file %s\n", dst_filename);
		return -1;
	}

	AVFormatContext* fmt_ctx = NULL;
	//open input media file, and allocate format context
	if ((err_code = avformat_open_input(&fmt_ctx, src_filename, NULL, NULL)) < 0) {
		av_strerror(err_code, errors, 1024);
		av_log(NULL, AV_LOG_DEBUG, "Could not open source file: %s, %d(%s)\n",
			src_filename,
			err_code,
			errors);
		return -1;
	}

	//retrieve audio stream
	if ((err_code = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
		av_strerror(err_code, errors, 1024);
		av_log(NULL, AV_LOG_DEBUG, "failed to find stream information: %s, %d(%s)\n",
			src_filename,
			err_code,
			errors);
		return -1;
	}

	//dump input information
	av_dump_format(fmt_ctx, 0, src_filename, 0);

	AVFrame* frame = NULL;
	AVPacket* pkt;
	int audio_stream_index = -1;
	int len;

	frame = av_frame_alloc();
	if (!frame) {
		av_log(NULL, AV_LOG_DEBUG, "Could not allocate frame\n");
		return AVERROR(ENOMEM);
	}

	//initialize packet
	//av_init_packet(&pkt);
	pkt = av_packet_alloc();
	pkt->data = NULL;
	pkt->size = 0;
	
	//find best audio stream
	audio_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (audio_stream_index < 0) {
		av_log(NULL, AV_LOG_DEBUG, "Could not find %s stream in input file %s\n",
			av_get_media_type_string(AVMEDIA_TYPE_AUDIO),
			src_filename);
		return AVERROR(EINVAL);
	}

	//read frame from media file
	while (av_read_frame(fmt_ctx, pkt) >= 0) {
		if (pkt->stream_index = audio_stream_index) {
			char adts_header_buf[7];
			adts_header(adts_header_buf, pkt->size);
			fwrite(adts_header_buf, 1, 7, dst_fd);

			len = fwrite(pkt->data, 1, pkt->size, dst_fd);
			if (len != pkt->size) {
				av_log(NULL, AV_LOG_DEBUG, "warning, length of writed data isn't equal pkt->size(%d, %d)\n",
					len,
					pkt->size);
			}
		}
		av_packet_unref(pkt);
	}

	//close input media file
	avformat_close_input(&fmt_ctx);
	if (dst_fd) {
		fclose(dst_fd);
	}

	return 0;
}

