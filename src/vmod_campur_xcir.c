#include <stdlib.h>
#include "vcl.h"
#include "vrt.h"
#include "bin/varnishd/cache.h"
#include <time.h>

#include <arpa/inet.h>
#include <syslog.h>
#include <poll.h>
	#include <fcntl.h>
	#include <sys/mman.h>
	#include <sys/types.h>
#include <stdio.h>

#include "vcc_if.h"
#include <mhash.h>



enum alphabets {
	BASE64 = 0,
	BASE64URL = 1,
	BASE64URLNOPAD = 2,
	N_ALPHA
};

static struct e_alphabet {
	char *b64;
	char i64[256];
	char padding;
} alphabet[N_ALPHA];
static void
vmod_digest_alpha_init(struct e_alphabet *alpha)
{
	int i;
	const char *p;

	for (i = 0; i < 256; i++)
		alpha->i64[i] = -1;
	for (p = alpha->b64, i = 0; *p; p++, i++)
		alpha->i64[(int)*p] = (char)i;
	if (alpha->padding)
		alpha->i64[alpha->padding] = 0;
}

int
init_function(struct vmod_priv *priv, const struct VCL_conf *conf)
{
    	alphabet[BASE64].b64 =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef"
		"ghijklmnopqrstuvwxyz0123456789+/";
	alphabet[BASE64].padding = '=';
	alphabet[BASE64URL].b64 =
		 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef"
		 "ghijklmnopqrstuvwxyz0123456789-_";
	alphabet[BASE64URL].padding = '=';
	alphabet[BASE64URLNOPAD].b64 =
		 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef"
		 "ghijklmnopqrstuvwxyz0123456789-_";
	alphabet[BASE64URLNOPAD].padding = 0;
	vmod_digest_alpha_init(&alphabet[BASE64]);
	vmod_digest_alpha_init(&alphabet[BASE64URL]);
	vmod_digest_alpha_init(&alphabet[BASE64URLNOPAD]);
	return (0);
}
static size_t
base64_encode (struct e_alphabet *alpha, const char *in,
		size_t inlen, char *out, size_t outlen)
{
	size_t outlenorig = outlen;
	unsigned char tmp[3], idx;

	if (outlen<4)
		return -1;

	if (inlen == 0) {
		*out = '\0';
		return (1);
	}

	while (1) {
		assert(inlen);
		assert(outlen>3);

		tmp[0] = (unsigned char) in[0];
		tmp[1] = (unsigned char) in[1];
		tmp[2] = (unsigned char) in[2];
		syslog(6,"[%c] [%c] [%c]",tmp[0],tmp[1],tmp[2]);
		*out++ = alpha->b64[(tmp[0] >> 2) & 0x3f];

		idx = (tmp[0] << 4);
		if (inlen>1)
			idx += (tmp[1] >> 4);
		idx &= 0x3f;
		*out++ = alpha->b64[idx];

		if (inlen>1) {
			idx = (tmp[1] << 2);
			if (inlen>2)
				idx += tmp[2] >> 6;
			idx &= 0x3f;

			*out++ = alpha->b64[idx];
		} else {
			if (alpha->padding)
				*out++ = alpha->padding;
		}

		if (inlen>2) {
			*out++ = alpha->b64[tmp[2] & 0x3f];
		} else {
			if (alpha->padding)
				*out++ = alpha->padding;
		}

		/*
		 * XXX: Only consume 4 bytes, but since we need a fifth for
		 * XXX: NULL later on, we might as well test here.
		 */
		if (outlen<5)
			return -1;

		outlen -= 4;

		if (inlen<4)
			break;

		inlen -= 3;
		in += 3;
	}

	assert(outlen);
	outlen--;
	*out = '\0';
	return outlenorig-outlen;
}


static const char *
vmod_base64_generic(struct sess *sp, enum alphabets a, const char *msg)
{
	char *p;
	int u;
	assert(msg);
	assert(a<N_ALPHA);
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->ws, WS_MAGIC);

	u = WS_Reserve(sp->ws,0);
	p = sp->ws->f;
	u = base64_encode(&alphabet[a],msg,strlen(msg),p,u);
	if (u < 0) {
		WS_Release(sp->ws,0);
		return NULL;
	}
	WS_Release(sp->ws,u);
	return p;
}


static const char *
vmod_hmac_generic(struct sess *sp, hashid hash, const char *key, const char *msg)
{
	size_t maclen = mhash_get_hash_pblock(hash);
	size_t blocksize = mhash_get_block_size(hash);
	unsigned char mac[blocksize];
	unsigned char *hexenc;
	unsigned char *hexptr;
	int j;
	MHASH td;

	assert(msg);
	assert(key);
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(sp->ws, WS_MAGIC);

	/*
	 * XXX: From mhash(3):
	 * size_t mhash_get_hash_pblock(hashid type);
	 *     It returns the block size that the algorithm operates. This
	 *     is used in mhash_hmac_init. If the return value is 0 you
	 *     shouldn't use that algorithm in  HMAC.
	 */
	assert(mhash_get_hash_pblock(hash) > 0);

	td = mhash_hmac_init(hash, (void *) key, strlen(key),
		mhash_get_hash_pblock(hash));
	mhash(td, msg, strlen(msg));
	mhash_hmac_deinit(td,mac);

	//base64encode
	hexenc = WS_Alloc(sp->ws, 64); // 0x, '\0' + 2 per input
	base64_encode(&alphabet[0],mac,blocksize,hexenc,64);
	return hexenc;
}

void vmod_aws_rest_generic(struct sess *sp,
	const char *accesskey,
	const char *secret,
	const char *method,
	const char *contentMD5,
	const char *contentType,
	const char *CanonicalizedAmzHeaders,
	const char *CanonicalizedResource,
	double date

){
	char buf[512];
	buf[0] = 0;
	syslog(6,"start");
	

	////////////////
	//gen date text
	char *datetxt;
	AN(datetxt = WS_Alloc(sp->http->ws, 32));
	struct tm tm;
	time_t tt;
	tt = (time_t) date;
	(void)gmtime_r(&tt, &tm);
	AN(strftime(datetxt, 32, "%a, %d %b %Y %T +0000", &tm));

	////////////////
	//build raw signature

	//method
	AN(method);
	strcat(buf,method);
	strcat(buf,"\n");
	
	//content-md5
	if(contentMD5)	strcat(buf,contentMD5);
	strcat(buf,"\n");
	
	//content-type
	if(contentType)	strcat(buf,contentType);
	strcat(buf,"\n");

	//date
	strcat(buf,datetxt);
	strcat(buf,"\n");

	//CanonicalizedAmzHeaders
	if(CanonicalizedAmzHeaders)	strcat(buf,CanonicalizedAmzHeaders);

	//CanonicalizedResource
	if(CanonicalizedResource)	strcat(buf,CanonicalizedResource);

	
	////////////////
	//build signature(HMAC-SHA1 + BASE64)
	const char* signature=vmod_hmac_generic(sp,MHASH_SHA1 , secret,buf);

	////////////////
	//set data
	VRT_SetHdr(sp, HDR_REQ, "\005Date:", datetxt,vrt_magic_string_end);
	const char* auth = VRT_WrkString(sp,"AWS ",accesskey,":",signature,vrt_magic_string_end);
	VRT_SetHdr(sp, HDR_REQ, "\016Authorization:", auth,vrt_magic_string_end);
	syslog(6,buf);
	syslog(6,signature);
	syslog(6,auth);
	
	syslog(6,"suc");
}

const char *
vmod_gethash(struct sess *sp){
	int size = DIGEST_LEN*2+1;
	if(WS_Reserve(sp->wrk->ws, 0) < size){
		WS_Release(sp->wrk->ws, 0);
		return NULL;
	}
	
	char *tmp = (char *)sp->wrk->ws->f;
	for(int i = 0; i < DIGEST_LEN; ++i)
		sprintf(tmp + 2 * i, "%02x",sp->digest[i]);
	WS_Release(sp->wrk->ws, size);
	return tmp;
}
double vmod_timeoffset(struct sess *sp, double time ,double os ,unsigned rev){
	if(rev){ os*=-1; }
	return time+os;
}

double vmod_timecmp(struct sess *sp,double time1,double time2){
	return(time1-time2);
}

struct sockaddr_storage * vmod_inet_pton(struct sess *sp,unsigned ipv6,const char *str,const char *defaultstr){
	int size = sizeof(struct sockaddr_storage);
	if(WS_Reserve(sp->wrk->ws, 0) < size){
		WS_Release(sp->wrk->ws, 0);
		return NULL;
	}
	struct sockaddr_storage *tmp = (char *)sp->wrk->ws->f;
	void *paddr;
	int ret = 0;
	int af;
	if(ipv6){
		tmp->ss_family = AF_INET6;
		paddr = &((struct sockaddr_in6 *)tmp)->sin6_addr;
		af = AF_INET6;
	}else{
		tmp->ss_family = AF_INET;
		paddr = &((struct sockaddr_in *)tmp)->sin_addr;
		af = AF_INET;
	}

	if(str != NULL){
		ret = inet_pton(af , str , paddr);
	}
	
	if(ret < 1){
		if(defaultstr == NULL){
			ret=inet_pton(af , "(:3[__])" , paddr);
		}else{
			ret=inet_pton(af , defaultstr , paddr);
		}
	}
	WS_Release(sp->wrk->ws, size);
	return tmp;
	
}

/*
double vmod_strptime(struct sess *sp,const char *str,const char *format){
	struct tm t;
	
	t.tm_year = 0;
	t.tm_mon  = 1;
	t.tm_mday = 1;
	t.tm_wday = 0;
	t.tm_hour = 0;
	t.tm_min  = 0;
	t.tm_sec  = 0;
	t.tm_isdst= -1;
	
	if(NULL==strptime(str, format, &t))
		return (double)mktime(&t);

	return 0;
}
*/