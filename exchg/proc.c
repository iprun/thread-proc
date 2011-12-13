#include <./util.h>
#include <./work.h>

#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>

// typedef struct
// {
// 	int fd;
// 	unsigned length; // overall length;
// 	unsigned eloffset;
// 	eltype * elements;
// 	unsigned elcount;
// } vector;

// static eltype * vels(const vector *const v)
// {
// 	return v->elements + v->eloffset;
// }

typedef struct
{
	int fd;
	unsigned offset; // in bytes
	unsigned length; // of data, the file length will be detected with lseek
} vectorfile;

static eltype vfelat(const vectorfile *const vf, const unsigned i)
{
	const unsigned elcnt = vf->length /sizeof(eltype);
	if(i < elcnt) {} else
	{
		fail("idx:%i is out of bounds", i);
	}

	off_t pos = vf->offset + i * sizeof(eltype);
	if(lseek(vf->fd, SEEK_SET, pos) == pos) {} else
	{
		fail("can't seek to pos:%u = off:%u + i:%u",
			pos, vf->offset, i * sizeof(eltype));
	}

	eltype value;
	if(read(vf->fd, &value, sizeof(value)) == sizeof(value)) {} else
	{
		fail("can't read");
	}

	return value;
}

typedef struct
{
	vectorfile vf;
	unsigned char padding[defpad(sizeof(vectorfile), cachelinelength)];
} elvector;

typedef struct
{
	char * ptr;
	unsigned capacity; // in bytes
	unsigned offset;
	unsigned length;
} vector;

#define actionfunction(fname) unsigned fname \
( \
	ringlink *const rl, \
	vectorfile *const vf, vector *const v, \
	const unsigned id, \
	const unsigned n \
)

// static unsigned expand(
// 	ringlink *const,
// 	vectorfile *const vf, vector *const v,
// 	const unsigned id,
// 	const unsigned n);
// 
// static unsigned shrink
// (
// 	ringlink *const,
// 	vectorfile *const, vector *const,
// 	const unsigned,
// 	const unsigned
// );
// 
// static unsigned exchange
// (
// 	ringlink *const,
// 	vector *const, 	const unsigned, const unsigned);

static actionfunction(expand);
static actionfunction(shrink);
static actionfunction(exchange);

// static unsigned (*const functions[])(
// 	vector *const, ringlink *const, const unsigned,
// 	const unsigned) =

static actionfunction((*const functions[])) =
{
	expand,
	shrink,
	exchange,
	expand,
	shrink,
	exchange,
	expand,
	shrink
};

const unsigned nfunctions = (sizeof(functions) / sizeof(void *));

static void routine
(
	ringlink *const rl, elvector *const vectors,
	const unsigned jid
) {
	const testconfig *const cfg = rl->cfg;
	const unsigned iters = cfg->niterations / cfg->nworkers;

	const int fd = vectors[jid].vf.fd;
	const unsigned len = flength(fd);	
	vector v = {
		.ptr = peekmap(cfg, fd, 0, len, pmwrite | pmprivate),
		.capacity = 0,
		.length = 0,
		.offset = 0 };

	unsigned seed = jid;
	unsigned id = jid;
	unsigned i;
	for(i = 0; id != (unsigned)-1 && i < iters; i += 1)
	{
			const unsigned r = rand_r(&seed);
			const unsigned fn = r % nfunctions;

			id = functions[fn](rl, &vectors[id].vf, &v, id, r);
	}

	uiwrite(rl->towrite, (unsigned)-1);

 	printf("unit %03u(%u) done. iters: %u; exchanges: %u\n",
 		jid, getpid(), i, rl->nexchanges);
}

static void runjobs
(
	const testconfig *const cfg,
	elvector *const vectors,
	void (*const code)(ringlink *const, elvector *const, unsigned)
) {
	const unsigned count = cfg->nworkers;
	pid_t procs[count];

	unsigned ok = 1;
	unsigned err = 0;

	int p0rd = -1;
	int p1rd = -1;
	int p1wr = -1;

	int zerowr = -1;

	unsigned i;

	// explicit external ifs would be too complex (if(count > 0) and so on).
	// Compiler should infer them during optimization
	for(i = 0; ok && i < count; i += 1)
	{
		if(i != 0)
		{
			p0rd = p1rd;
		}
		else
		{
			makerlink(&zerowr, &p0rd);
		}

		if(i != count - 1)
		{
			makerlink(&p1wr, &p1rd);
		}
		else
		{
			p1wr = zerowr;
		}

		pid_t p = fork();
		if(p == 0)
		{
			ringlink rl = {
				.toread = p0rd,
				.towrite = p1wr,
				.nexchanges = 0,
				.writable = 1,
				.cfg = cfg };

			if(i != count - 1)
			{
				uclose(zerowr);
				uclose(p1rd);
			}

			code(&rl, vectors, i);
			exit(0);
		}
		else if(p > 0)
		{
			procs[i] = p;

			uclose(p0rd);
			uclose(p1wr);
		}
		else
		{
			ok = 0;
		}

		ok = err == 0;
	}

	if(ok) {} else
	{
		fail("can't start %u jobs", count);
	}

	printf("%u jobs stated\n", i);

	for(i = 0; ok && i < count; i += 1)
	{
		const pid_t p = waitpid(procs[i], NULL, 0);
		ok = p == procs[i];
	}

	if(ok) {} else
	{
		fail("can't join %u jobs", count);
	}

	printf("%u jobs joined\n", i); 
}

int main(const int argc, const char *const *const argv)
{
	ignoresigpipe();

	const testconfig cfg = fillconfig(argc, argv);
	const unsigned vectslen = sizeof(elvector) * cfg.nworkers;
	
	elvector *const vectors
		= (elvector *)peekmap(&cfg, -1, 0, vectslen, pmwrite);

	for(unsigned i = 0; i < cfg.nworkers; i += 1)
	{
		// makeshm will align 1 up to pagelength
		vectors[i].vf.fd = makeshm(&cfg, 1);
		vectors[i].vf.length = 0;
		vectors[i].vf.offset = 0;
	}

	fflush(stderr);
	fflush(stdout);
	runjobs(&cfg, vectors, routine);

	printf("some values\n");

	for(unsigned i = 0; i < cfg.nworkers; i += 1)
	{
		if(vectors[i].vf.length)
		{
			printf("\t%f\n", (double)vfelat(&vectors[i].vf, 0));
		}
		else
		{
			printf("\tEMPTY\n");
		}
	}

	printf("DONE. vector size: %lu; elvector size %lu\n",
		(unsigned long)sizeof(vector),
		(unsigned long)sizeof(elvector));	

	dropmap(&cfg, vectors, vectslen);

	return 0;
}

// static unsigned expand(
// 	vector *const v, ringlink *const rl,
// 	const unsigned id, const unsigned r)

actionfunction(expand)
{
// 	const unsigned n = r % workfactor;
// 	unsigned seed = r;
// 
// 	array.reserve(array.size() + n);
// 	for(unsigned i = 0; i < n; i += 1)
// 	{
// 		array.push_back(elrand(&seed));
// 	}

	return id;
}

// static unsigned shrink(
// 	vector *const v, ringlink *const rl,
// 	const unsigned id, const unsigned r)

actionfunction(shrink)
{
// 	const unsigned n = min((unsigned long)(r % workfactor), array.size());
// 
// 	if(n > 0)
// 	{
// 		array.push_back(heapsum(&array[0], n));
// 
// 		vector<eltype>::iterator b = array.begin();
// 		array.erase(b, b + n);
// 	}

	return id;
}

// static unsigned exchange(
// 	vector *const v, ringlink *const rl,
// 	const unsigned id, const unsigned n)

actionfunction(exchange)
{
	rl->nexchanges += 1;

	if(rl->writable)
	{
		rl->writable = uiwrite(rl->towrite, id);
	}

	return uiread(rl->toread);
}
