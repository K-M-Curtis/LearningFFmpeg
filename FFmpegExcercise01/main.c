#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libavcodec/avcodec.h>

int main(int argc, char* argv[]) {
	printf("test FFmpeg...\n");
	const char* s = avcodec_configuration();
	printf("%s\n", s);	

	system("pause");
	return 0;
}