/* Copy of openpak/ps3-patcher plugin/catcher.h (v0.2.17): the console DNS catcher core, run unchanged by Pier. */
/*
 * catcher.h -- the OpenPak DNS catcher core, shared by the VSH plugin, the host test
 * (tools/test_catcher.c) and Pier's emulated hook. Freestanding C: no libc, no allocation.
 *
 * Where it sits. The PS3 resolver in libnet.sprx is BIND's res_send(), which keeps BIND's query
 * hook: before each query goes to a nameserver, res_send() calls the function in its Qhook slot
 * (set by res_send_setqhook(), libnet 4.93 text+0x9464) as
 *
 *     int qhook(struct sockaddr_in **ns, const u_char **query, int *querylen,
 *               u_char *ans, int anssiz, int *resplen);
 *
 * and on res_done (3) returns *resplen bytes of ans as the reply without sending anything. So a
 * lookup whose name matches a family on the list is answered here with the OpenPak box address;
 * every other lookup gets res_goahead (0) and goes to the console's own DNS exactly as before.
 * Every lookup in the process goes through res_send(): gethostbyname and the async
 * cellNetDbResolver both end there (proven on Pier, see docs/dns-catcher.md).
 *
 * The list is entries of three kinds: a family ("playstation.net" covers the apex and every name
 * below it), an exact name, or a never-name that is passed through even inside a family.
 */
#ifndef OPENPAK_CATCHER_H
#define OPENPAK_CATCHER_H

#define CATCHER_MAX 64      /* entries */
#define CATCHER_LEN 96      /* bytes per entry, NUL included */
#define CATCHER_NAME 256    /* longest decoded query name, NUL included */

#define CATCHER_FAMILY 0
#define CATCHER_EXACT  1
#define CATCHER_NEVER  2

#define CATCHER_GOAHEAD 0   /* BIND res_goahead */
#define CATCHER_DONE    3   /* BIND res_done */

typedef struct {
	unsigned char ip[4];                      /* answer address, network order */
	int n;
	unsigned char kind[CATCHER_MAX];
	char name[CATCHER_MAX][CATCHER_LEN];      /* lower case, no leading dot */
} catcher_list;

static int catcher_lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Adds one entry. A leading "." or "*." is dropped. Returns 0 when it was refused (full,
 * too long, empty, fewer than two labels for a family, or characters outside a host name). */
static int catcher_add(catcher_list *l, int kind, const char *s, int len)
{
	int i, dots = 0, k = 0;
	if (len > 1 && s[0] == '*' && s[1] == '.') { s += 2; len -= 2; }
	else if (len > 0 && s[0] == '.') { s++; len--; }
	if (len <= 0 || len >= CATCHER_LEN || l->n >= CATCHER_MAX) return 0;
	for (i = 0; i < len; i++) {
		int c = catcher_lc((unsigned char)s[i]);
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return 0;
		if (c == '.') { if (i == 0 || i == len - 1 || s[i - 1] == '.') return 0; dots++; }
		l->name[l->n][k++] = (char)c;
	}
	if (kind == CATCHER_FAMILY && dots < 1) return 0;
	l->name[l->n][k] = 0;
	l->kind[l->n] = (unsigned char)kind;
	l->n++;
	return 1;
}

/* name ends with "." + fam, or equals fam (both lower case). */
static int catcher_under(const char *name, int nlen, const char *fam)
{
	int flen = 0, i;
	while (fam[flen]) flen++;
	if (nlen < flen) return 0;
	for (i = 0; i < flen; i++) if (name[nlen - flen + i] != fam[i]) return 0;
	return nlen == flen || name[nlen - flen - 1] == '.';
}

/* Index of the entry that catches name (lower case), or -1 when the name passes through. */
static int catcher_match(const catcher_list *l, const char *name)
{
	int nlen = 0, i, hit = -1;
	while (name[nlen]) nlen++;
	if (nlen > 0 && name[nlen - 1] == '.') nlen--;     /* fully qualified form */
	for (i = 0; i < l->n; i++) {
		if (l->kind[i] == CATCHER_NEVER) {
			int k = 0, same = 1;
			while (l->name[i][k]) k++;
			if (k != nlen) continue;
			for (k = 0; k < nlen; k++) if (l->name[i][k] != name[k]) { same = 0; break; }
			if (same) return -1;
		}
	}
	for (i = 0; i < l->n && hit < 0; i++) {
		if (l->kind[i] == CATCHER_FAMILY && catcher_under(name, nlen, l->name[i])) hit = i;
		else if (l->kind[i] == CATCHER_EXACT) {
			int k = 0, same = 1;
			while (l->name[i][k]) k++;
			if (k != nlen) continue;
			for (k = 0; k < nlen; k++) if (l->name[i][k] != name[k]) { same = 0; break; }
			if (same) hit = i;
		}
	}
	return hit;
}

/* Decodes the single question of a DNS query: the name (lower case, dotted), its type and class,
 * and the offset just past the question. Returns 0 for anything that is not a plain one-question
 * query (those always pass through). */
static int catcher_question(const unsigned char *q, int qlen, char *name, int *qtype, int *qclass, int *qend)
{
	int p = 12, n = 0;
	if (!q || qlen < 17 || qlen > 4096) return 0;
	if (q[2] & 0x80) return 0;                          /* a response, not a query */
	if (((q[2] >> 3) & 15) != 0) return 0;              /* opcode QUERY only */
	if (q[4] != 0 || q[5] != 1) return 0;               /* exactly one question */
	for (;;) {
		int len, i;
		if (p >= qlen) return 0;
		len = q[p++];
		if (len == 0) break;
		if (len > 63 || p + len > qlen || n + len + 2 > CATCHER_NAME) return 0;
		if (n) name[n++] = '.';
		for (i = 0; i < len; i++) {
			int c = catcher_lc(q[p + i]);
			if (c == 0 || c == '.') return 0;
			name[n++] = (char)c;
		}
		p += len;
	}
	if (n == 0 || p + 4 > qlen) return 0;
	name[n] = 0;
	*qtype = (q[p] << 8) | q[p + 1];
	*qclass = (q[p + 2] << 8) | q[p + 3];
	*qend = p + 4;
	return 1;
}

/* Writes the reply to q into ans: the question echoed, recursion available, NOERROR, and one A
 * record (TTL 60) for type A / ANY in class IN; any other type gets an empty NOERROR answer, so
 * nothing about a caught name ever reaches another server. Returns the length, or 0 if ans is too
 * small. */
static int catcher_answer(const unsigned char *q, int qend, int qtype, int qclass,
			  const unsigned char ip[4], unsigned char *ans, int anssiz)
{
	int i, n = qend, a = (qclass == 1 && (qtype == 1 || qtype == 255));
	if (anssiz < qend + (a ? 16 : 0)) return 0;
	for (i = 0; i < qend; i++) ans[i] = q[i];
	ans[2] = (unsigned char)(0x80 | (q[2] & 0x79));      /* QR, opcode, RD kept; AA, TC clear */
	ans[3] = 0x80;                                       /* RA, rcode NOERROR */
	ans[6] = 0; ans[7] = (unsigned char)a;               /* ANCOUNT */
	ans[8] = ans[9] = ans[10] = ans[11] = 0;             /* no authority, no additional (EDNS dropped) */
	if (a) {
		static const unsigned char rr[12] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
		for (i = 0; i < 12; i++) ans[n++] = rr[i];
		for (i = 0; i < 4; i++) ans[n++] = ip[i];
	}
	return n;
}

/* The whole decision for one query. Fills name (always, when the query decodes) and returns the
 * reply length (> 0: caught, ans holds the answer) or 0 (pass through). *hit is the entry index. */
static int catcher_handle(const catcher_list *l, const unsigned char *q, int qlen,
			  unsigned char *ans, int anssiz, char *name, int *hit)
{
	int qtype, qclass, qend;
	*hit = -1;
	name[0] = 0;
	if (!l || l->n == 0) return 0;
	if (!catcher_question(q, qlen, name, &qtype, &qclass, &qend)) return 0;
	*hit = catcher_match(l, name);
	if (*hit < 0) return 0;
	return catcher_answer(q, qend, qtype, qclass, l->ip, ans, anssiz);
}

#endif
