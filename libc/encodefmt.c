#include <u.h>
#include <libc.h>
#include <ctype.h>

int
encodefmt(Fmt *f)
{
	char *out;
	char *buf;
	uint len;
	uint ilen;
	int rv;
	uchar *b;
	char *p;
	char obuf[64];	// rsc optimization

	if(!(f->flags&FmtPrec) || f->prec < 1)
		goto error;

	b = va_arg(f->args, uchar*);
	if(b == 0)
		return fmtstrcpy(f, "<nil>");

	ilen = f->prec;
	f->prec = 0;
	f->flags &= ~FmtPrec;
	switch(f->r){
	case '<':
		len = (8*ilen+4)/5 + 3;
		break;
	case '[':
		len = (8*ilen+5)/6 + 4;
		break;
	case 'H':
		len = 2*ilen + 1;
		break;
	default:
		goto error;
	}

	if(len > sizeof(obuf)){
		buf = malloc(len);
		if(buf == nil)
			goto error;
	} else
		buf = obuf;

	// convert
	out = buf;
	switch(f->r){
	case '[':
		rv = enc64(out, len, b, ilen);
		break;
	case '<':
		rv = enc32(out, len, b, ilen);
		break;
	case 'H':
		rv = enc16(out, len, b, ilen);
		break;
	default:
		rv = -1;
		break;
	}
	if(rv < 0)
		goto error;

	if((f->flags & FmtLong) != 0 && f->r != '[')
		for(p = buf; *p; p++)
			*p = tolower(*p);

	fmtstrcpy(f, buf);
	if(buf != obuf)
		free(buf);
	return 0;

error:
	return fmtstrcpy(f, "<encodefmt>");
}
