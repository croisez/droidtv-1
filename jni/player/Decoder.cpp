#include "Decoder.h"
#include "utils.h"

#include <pthread.h>

Decoder::Decoder(AVStream* stream) {
	pStream = stream;
	pAbort = false;
	mRunning = false;
	pthread_mutex_init(&mQueue.mutex, NULL);
	pthread_cond_init(&mQueue.cond, NULL);
	mQueue.first = NULL;
	mQueue.last = NULL;
	mQueue.size = 0;
}

Decoder::~Decoder() {
	if (mRunning) {
		stop();
	}

	avcodec_close(pStream->codec);
	pthread_mutex_destroy(&mQueue.mutex);
	pthread_cond_destroy(&mQueue.cond);

	// free queue
	AVPacketList *pkt, *pkt1;
	for (pkt = mQueue.first; pkt != NULL; pkt = pkt1) {
		av_free_packet(&pkt->pkt);
		pkt1 = pkt->next;
		av_freep(&pkt);
	}
}

int Decoder::packets() {
	return mQueue.size;
}

int Decoder::enqueue(AVPacket* packet) {
	int ret;
	if ((ret = av_dup_packet(packet)) < 0) {
		LOGE("Decoder::enqueue=%d", ret);
		return -1; // TODO specify error
	}

	AVPacketList* pkt = (AVPacketList *) av_malloc(sizeof(AVPacketList));
	if (pkt == NULL) {
		LOGE("Couldn't allocate AVPacketList");
		return -1; // TODO specify error
	}

	pkt->pkt = *packet;
	pkt->next = NULL;

	pthread_mutex_lock(&mQueue.mutex);

	if (mQueue.last == NULL) {
		mQueue.first = pkt;
	} else {
		mQueue.last->next = pkt;
	}
	mQueue.last = pkt;
	mQueue.size++;

	pthread_cond_signal(&mQueue.cond);
	pthread_mutex_unlock(&mQueue.mutex);

	return 0;
}

void Decoder::dequeue(AVPacket* packet) {
	AVPacketList* pkt;
	pthread_mutex_lock(&mQueue.mutex);
	for (;;) {
		pkt = mQueue.first;
		if (pkt == NULL) {
			LOGD("Decoder::dequeue: empty queue, waiting...");
			pthread_cond_wait(&mQueue.cond, &mQueue.mutex);
		} else {
			mQueue.first = pkt->next;
			if (mQueue.first == NULL) {
				mQueue.last = NULL;
			}
			mQueue.size--;
			*packet = pkt->pkt;
			av_free(pkt);
			break;
		}
	}
	pthread_mutex_unlock(&mQueue.mutex);
}

void Decoder::start() {
	struct sched_param param = { 10 }; // java:Thread.MAX_PRIORITY
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setschedparam(&attr, &param);
	pthread_create(&mThread, &attr, runThread, this);
}

void* Decoder::runThread(void* ptr) {
	JNIEnv *env;
	getJVM()->AttachCurrentThread(&env, NULL);
	LOGI("starting decoder thread");
	Decoder* decoder = (Decoder*) ptr;
	decoder->mRunning = true;
	if (decoder->prepare()) {
		decoder->decode();
	} else {
		LOGE("Couldn't prepare decoder");
	}
	decoder->mRunning = false;
	LOGI("decoder thread ended");
	getJVM()->DetachCurrentThread();
	return NULL;
}

void Decoder::stop() {
	if(!mRunning) {
		return;
	}

	pthread_mutex_lock(&mQueue.mutex);
	pAbort = true;
	pthread_cond_signal(&mQueue.cond);
	pthread_mutex_unlock(&mQueue.mutex);

	LOGI("waiting on end of decoder thread");
	int ret;
	if ((ret = wait()) != 0) {
		LOGE("Couldn't cancel decoder thread: %i", ret);
		return;
	}
}

int Decoder::wait() {
	if (!mRunning) {
		return 0;
	}
	return pthread_join(mThread, NULL);
}