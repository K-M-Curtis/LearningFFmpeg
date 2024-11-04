// FFmpeg5.1 的 audio decoding 例子
// 解码音频数据, 输入aac格式的文件.最后生成pcm格式的文件
// 每次从文件中读取一部分数据(20480), 然后把这部分数据解析成packet.然后在送入解码器解码成frame.
// 最后写到文件中, 然后在继续从文件中读取数据, 解析packet.生成frame. 
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/frame.h>
#include <libavutil/mem.h>

#include <libavcodec/avcodec.h>

#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096

static int get_format_from_sample_fmt(const char** fmt, enum AVSampleFormat sample_fmt) {
	struct sample_fmt_entry {
		enum AVSampleFormat sample_fmt; const char* fmt_be; const char* fmt_le;
	} sample_fmt_entries[] = {
		{ AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
		{ AV_SAMPLE_FMT_S16, "s16be", "s16le" },
		{ AV_SAMPLE_FMT_S32, "s32be", "s32le" },
		{ AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
		{ AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
	};

	*fmt = NULL;
	for (int i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
		struct sample_fmt_entry* entry = &sample_fmt_entries[i];
		if (sample_fmt == entry->sample_fmt) {
			*fmt = AV_NE(entry->fmt_be, entry->fmt_le);
			return 0;
		}
	}
	fprintf(stderr,
		"sample format %s is not supported as output format\n",
		av_get_sample_fmt_name(sample_fmt));
	return -1;
}

static void decode(AVCodecContext* dec_ctx, AVPacket* pkt, AVFrame* frame, FILE* outfile) {
	int ret, data_size;

	/* send the packet with the compressed data to the decoder */
	ret = avcodec_send_packet(dec_ctx, pkt);
	if (ret < 0) {
		fprintf(stderr, "Error submitting the packet to the decoder\n");
		exit(1);
	}

	/* read all the output frames (in general there may be any number of them */
	while (ret >= 0) {
		// 获取解码出的frame,一个packet可能会解码出很多的frame. 
		ret = avcodec_receive_frame(dec_ctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			return;
		}
		else if (ret < 0) {
			fprintf(stderr, "Error during decoding\n");
			exit(1);
		}
		data_size = av_get_bytes_per_sample(dec_ctx->sample_fmt);
		if (data_size < 0) {
			/* This should not occur, checking just for paranoia */
			fprintf(stderr, "Failed to calculate data size\n");
			exit(1);
		}
		//每个声道的样本数
		for (int i = 0; i < frame->nb_samples; i++) {
			//所有的通道
			for (int ch = 0; ch < dec_ctx->channels; ch++) {
				fwrite(frame->data[ch] + data_size * i, 1, data_size, outfile);
			}
		}
	}
}

int main(int argc, char* argv[]) {
	const AVCodec* codec;
	AVCodecContext* codec_ctx = NULL;
	//用于解析输入的数据流并把它分成一帧一帧的压缩编码数据。
	AVCodecParserContext* parser = NULL;
	//加上AV_INPUT_BUFFER_PADDING_SIZE是为了防止某些优化过的reader一次性读取过多导致越界。
	//然后调用fread函数从本地文件中每次读取AUDIO_INBUF_SIZE大小的数据到缓存区中。
	uint8_t inbuf[AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
	uint8_t* data;
	size_t data_size;
	AVPacket* pkt;
	AVFrame* decoded_frame = NULL;
	enum AVSampleFormat sfmt;
	int n_channels = 0;
	const char* fmt;
	int ret;

	if (argc <= 2) {
		fprintf(stderr, "Usage: %s <input file> <output file>\n", argv[0]);
		exit(0);
	}

	const char* in_filename = argv[1];
	const char* out_filename = argv[2];

	pkt = av_packet_alloc();

	/* find the MPEG audio decoder */
	//找到解码器,指定类型的音频解码器 这里指的AAC 因为输入文件是aac
	codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
	if (!codec) {
		fprintf(stderr, "Codec not found\n");
		exit(1);
	}

	parser = av_parser_init(codec->id);
	if (!parser) {
		fprintf(stderr, "Parser not found\n");
		exit(1);
	}

	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx) {
		fprintf(stderr, "Could not allocate audio codec context\n");
		exit(1);
	}

	/* open it */
	if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
		fprintf(stderr, "Could not open codec\n");
		exit(1);
	}

	FILE* in_fp = fopen(in_filename, "rb");
	if (!in_fp) {
		fprintf(stderr, "Could not open %s\n", in_filename);
		exit(1);
	}

	FILE* out_fp = fopen(out_filename, "wb");
	if (!out_fp) {
		av_free(codec_ctx);
		exit(1);
	}

	/* decode until eof */
	data = inbuf;
	data_size = fread(inbuf, 1, AUDIO_INBUF_SIZE, in_fp);

	while (data_size > 0) {
		if (!decoded_frame) {
			if (!(decoded_frame = av_frame_alloc())) {
				fprintf(stderr, "Could not allocate audio frame\n");
				exit(1);
			}
		}

		ret = av_parser_parse2(parser, codec_ctx, &pkt->data, &pkt->size, data, data_size,
			AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
		if (ret < 0) {
			fprintf(stderr, "Error while parsing\n");
			exit(1);
		}
		data += ret;
		data_size -= ret;

		if (pkt->size) {
			decode(codec_ctx, pkt, decoded_frame, out_fp);
		}

		//这里表示data剩余的数据已经小于 4096这个阈值.需要继续从文件中读取一部分数据，如图所示
		if (data_size < AUDIO_REFILL_THRESH) {
			// memmove用于拷贝字节，如果目标区域和源区域有重叠的话，memmove能够保证源串在被覆盖之前将重叠区域的字节
			// 拷贝到目标区域中，但复制后源内容会被更改。但是当目标区域与源区域没有重叠则和memcpy函数功能相同。
			memmove(inbuf, data, data_size);
			data = inbuf;
			int len = fread(data + data_size, 1, AUDIO_INBUF_SIZE - data_size, in_fp);
			if (len > 0) {
				data_size += len;
			}
		}
	}

	/* flush the decoder */
	//因为codec可能在内部缓冲多个frame或packet，出于性能或其他必要的情况（如考虑B帧的情况）
	//步骤如下：
	//1.调用avcodec_send_*()传入的AVFrame或AVPacket指针设置为NULL。 这将开启draining mode（排水模式）;
	//2.反复地调用avcodec_receive_*()直到返回AVERROR_EOF的错误;
	//3.codec可以重新开启，但是需要先调用 avcodec_flush_buffers()来重置codec
	pkt->data = NULL;
	pkt->size = 0;
	decode(codec_ctx, pkt, decoded_frame, out_fp);

	/* print output pcm infomations, because there have no metadata of pcm */
	sfmt = codec_ctx->sample_fmt;
	if (av_sample_fmt_is_planar(sfmt)) {
		const char* packed = av_get_sample_fmt_name(sfmt);
		printf("Warning: the sample format the decoder produced is planar "
			"(%s). This example will output the first channel only.\n",
			packed ? packed : "?");
		sfmt = av_get_packed_sample_fmt(sfmt);
	}

	/* FFmpeg 5.0 */
	//n_channels = codec_ctx->channels;
	n_channels = codec_ctx->ch_layout.nb_channels;
	if ((ret = get_format_from_sample_fmt(&fmt, sfmt)) < 0)
		goto end;
	printf("Play the output audio file with the command:\n"
		"ffplay -f %s -ac %d -ar %d %s\n",
		fmt, n_channels, codec_ctx->sample_rate,
		out_filename);

end:
	fclose(out_fp);
	fclose(in_fp);

	avcodec_free_context(&codec_ctx);
	av_parser_close(parser);
	av_frame_free(&decoded_frame);
	av_packet_free(&pkt);

	return 0;
}