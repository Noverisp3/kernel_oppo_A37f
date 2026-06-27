#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include <linux/slab.h>

#define FRANDOM_VERSION "1.1"
#define RANDSIZ 256

struct frandom_ctx {
	u32 rand[RANDSIZ];
	u32 mm[RANDSIZ];
	u32 aa, bb, cc;
	int cnt;
};

static void isaac_update(struct frandom_ctx *ctx)
{
	u32 a = ctx->aa, b = ctx->bb, c = ctx->cc + 1;
	u32 x, y, *m = ctx->mm, *r = ctx->rand;
	u32 *m2 = m + RANDSIZ / 2, *m_end = m + RANDSIZ;

	ctx->cc = c;
	ctx->bb = ++b;

	for (; m < m_end; m += 2, m2 += 2, r += 2) {
		x = m[0];
		m[0] = a ^ (a << 13) ^ m2[0];
		y = m2[0] + ctx->mm[(m2[0] >> 2) & (RANDSIZ - 1)];
		r[0] = y + m[0];
		a = m[0] ^ b;
		b = x + y;

		x = m[1];
		m[1] = a ^ (a >> 6) ^ m2[1];
		y = m2[1] + ctx->mm[(m2[1] >> 2) & (RANDSIZ - 1)];
		r[1] = y + m[1];
		a = m[1] ^ b;
		b = x + y;
	}

	for (m2 = ctx->mm; m2 < ctx->mm + RANDSIZ / 2; m += 2, m2 += 2, r += 2) {
		x = m[0];
		m[0] = a ^ (a << 2) ^ m2[0];
		y = m2[0] + ctx->mm[(m2[0] >> 2) & (RANDSIZ - 1)];
		r[0] = y + m[0];
		a = m[0] ^ b;
		b = x + y;

		x = m[1];
		m[1] = a ^ (a >> 16) ^ m2[1];
		y = m2[1] + ctx->mm[(m2[1] >> 2) & (RANDSIZ - 1)];
		r[1] = y + m[1];
		a = m[1] ^ b;
		b = x + y;
	}

	ctx->aa = a;
	ctx->bb = b;
	ctx->cnt = RANDSIZ;
}

static void mix(u32 *a, u32 *b, u32 *c, u32 *d, u32 *e, u32 *f, u32 *g, u32 *h)
{
	*a ^= *d << 11; *d += *a; *b += *c;
	*b ^= *e >> 2;  *e += *b; *c += *d;
	*c ^= *f << 8;  *f += *c; *d += *e;
	*d ^= *g >> 16; *g += *d; *e += *f;
	*e ^= *h << 10; *h += *e; *f += *g;
	*f ^= *a >> 4;  *a += *f; *g += *h;
	*g ^= *b << 8;  *b += *g; *h += *a;
	*h ^= *d >> 9;  *c += *h; *a += *b;
}

static void isaac_init(struct frandom_ctx *ctx, u32 *seed, int n)
{
	u32 a, b, c, d, e, f, g, h;
	int i;

	a = b = c = d = e = f = g = h = 0x9e3779b9;

	memset(ctx->mm, 0, sizeof(ctx->mm));

	for (i = 0; i < 4; i++)
		mix(&a, &b, &c, &d, &e, &f, &g, &h);

	for (i = 0; i < RANDSIZ; i += 8) {
		a += ctx->mm[i]; b += ctx->mm[i + 1];
		c += ctx->mm[i + 2]; d += ctx->mm[i + 3];
		e += ctx->mm[i + 4]; f += ctx->mm[i + 5];
		g += ctx->mm[i + 6]; h += ctx->mm[i + 7];
		mix(&a, &b, &c, &d, &e, &f, &g, &h);
		ctx->mm[i] = a; ctx->mm[i + 1] = b;
		ctx->mm[i + 2] = c; ctx->mm[i + 3] = d;
		ctx->mm[i + 4] = e; ctx->mm[i + 5] = f;
		ctx->mm[i + 6] = g; ctx->mm[i + 7] = h;
	}

	if (seed) {
		for (i = 0; i < RANDSIZ; i += 8) {
			a += seed[i % n]; b += seed[(i + 1) % n];
			c += seed[(i + 2) % n]; d += seed[(i + 3) % n];
			e += seed[(i + 4) % n]; f += seed[(i + 5) % n];
			g += seed[(i + 6) % n]; h += seed[(i + 7) % n];
			mix(&a, &b, &c, &d, &e, &f, &g, &h);
			ctx->mm[i] = a; ctx->mm[i + 1] = b;
			ctx->mm[i + 2] = c; ctx->mm[i + 3] = d;
			ctx->mm[i + 4] = e; ctx->mm[i + 5] = f;
			ctx->mm[i + 6] = g; ctx->mm[i + 7] = h;
		}
	}

	ctx->aa = a;
	ctx->bb = b;
	ctx->cc = 0;

	isaac_update(ctx);
	ctx->cnt = RANDSIZ;
}

static struct frandom_ctx *frandom_ctx;
static DEFINE_MUTEX(frandom_mutex);

static u32 get_fast_random(void)
{
	u32 ret;

	mutex_lock(&frandom_mutex);
	if (frandom_ctx->cnt <= 0)
		isaac_update(frandom_ctx);
	ret = frandom_ctx->rand[--frandom_ctx->cnt];
	mutex_unlock(&frandom_mutex);
	return ret;
}

static void seed_frandom(void)
{
	u32 seed[8];
	int i;

	get_random_bytes(seed, sizeof(seed));
	for (i = 0; i < 8; i++)
		seed[i] ^= (u32)get_cycles();

	isaac_init(frandom_ctx, seed, 8);
}

static ssize_t frandom_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	size_t i;
	u32 tmp;

	for (i = 0; i + sizeof(u32) <= count; i += sizeof(u32)) {
		tmp = get_fast_random();
		if (copy_to_user(buf + i, &tmp, sizeof(u32)))
			return -EFAULT;
	}
	if (i < count) {
		tmp = get_fast_random();
		if (copy_to_user(buf + i, &tmp, count - i))
			return -EFAULT;
	}
	return count;
}

static int frandom_open(struct inode *inode, struct file *file)
	{ return 0; }
static int frandom_release(struct inode *inode, struct file *file)
	{ return 0; }

static const struct file_operations frandom_fops = {
	.owner		= THIS_MODULE,
	.read		= frandom_read,
	.open		= frandom_open,
	.release	= frandom_release,
};

static struct miscdevice frandom_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "frandom",
	.fops	= &frandom_fops,
};

static ssize_t erandom_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	size_t i;
	u32 tmp;

	for (i = 0; i + sizeof(u32) <= count; i += sizeof(u32)) {
		get_random_bytes(&tmp, sizeof(tmp));
		if (copy_to_user(buf + i, &tmp, sizeof(u32)))
			return -EFAULT;
	}
	if (i < count) {
		get_random_bytes(&tmp, sizeof(tmp));
		if (copy_to_user(buf + i, &tmp, count - i))
			return -EFAULT;
	}
	return count;
}

static const struct file_operations erandom_fops = {
	.owner	= THIS_MODULE,
	.read	= erandom_read,
	.open	= frandom_open,
	.release= frandom_release,
};

static struct miscdevice erandom_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "erandom",
	.fops	= &erandom_fops,
};

static int __init frandom_init(void)
{
	int ret;

	frandom_ctx = kzalloc(sizeof(struct frandom_ctx), GFP_KERNEL);
	if (!frandom_ctx)
		return -ENOMEM;

	seed_frandom();

	ret = misc_register(&frandom_misc);
	if (ret) {
		pr_err("frandom: misc_register failed\n");
		goto err_free;
	}

	ret = misc_register(&erandom_misc);
	if (ret) {
		pr_err("frandom: erandom misc_register failed\n");
		goto err_unreg;
	}

	pr_info("frandom: version %s loaded\n", FRANDOM_VERSION);
	return 0;

err_unreg:
	misc_deregister(&frandom_misc);
err_free:
	kfree(frandom_ctx);
	return ret;
}

static void __exit frandom_exit(void)
{
	misc_deregister(&frandom_misc);
	misc_deregister(&erandom_misc);
	kfree(frandom_ctx);
	pr_info("frandom: unloaded\n");
}

module_init(frandom_init);
module_exit(frandom_exit);

MODULE_LICENSE("GPL");
MODULE_VERSION(FRANDOM_VERSION);
MODULE_AUTHOR("Noveris");
MODULE_DESCRIPTION("Fast Random Number Generator (ISAAC PRNG)");
