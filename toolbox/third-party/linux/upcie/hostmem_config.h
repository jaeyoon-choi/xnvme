#ifndef MFD_HUGE_2MB
#define MFD_HUGE_2MB (21 << 26)
#endif
#ifndef MFD_HUGE_1GB
#define MFD_HUGE_1GB (30 << 26)
#endif

/**
 * A representation of a various host memory properties, primarly for hugepages
 */
struct hostmem_config {
	char hugetlb_path[128]; ///< Mountpoint of hugetlbsfs
	int memfd_flags;        ///< Flags for memfd_create(...)
	int backend;
	int count;
	int use_hugepages; ///< Whether allocations should use hugetlb-backed pages
	int pagesize; ///< Host memory pagesize (not HUGEPAGE size)
	int pagesize_shift;
	int hugepgsz; ///< THIS, is the HUGEPAGE size
};

static inline int
hostmem_config_pp(struct hostmem_config *config)
{
	int wrtn = 0;

	wrtn += printf("hostmem:");

	if (!config) {
		wrtn += printf(" ~\n");
		return 0;
	}

	wrtn += printf("  \n");
	wrtn += printf("  hugetlb_path: '%s'\n", config->hugetlb_path);
	wrtn += printf("  memfd_flags: 0x%x\n", config->memfd_flags);
	wrtn += printf("  backend: 0x%x\n", config->backend);
	wrtn += printf("  count: %d\n", config->count);
	wrtn += printf("  use_hugepages: %d\n", config->use_hugepages);
	wrtn += printf("  pagesize: %d\n", config->pagesize);
	wrtn += printf("  pagesize_shift: %d\n", config->pagesize_shift);
	wrtn += printf("  hugepgsz: %d\n", config->hugepgsz);

	return wrtn;
};

static inline int
hostmem_config_get_hugepgsz(int *hugepgsz)
{
	char line[256];
	FILE *fp;

	fp = fopen("/proc/meminfo", "r");
	if (!fp) {
		UPCIE_DEBUG("FAIELD: fopen(meminfo); errno(%d)", errno);
		return -errno;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "Hugepagesize:", 13) == 0) {
			int kb;
			if (sscanf(line + 13, "%d", &kb) == 1) {
				fclose(fp);
				*hugepgsz = kb * 1024;
				return 0;
			}
		}
	}

	fclose(fp);
	return -ENOMEM;
}

static inline int
hostmem_env_is_true(const char *env)
{
	return env && env[0] && strcmp(env, "0") && strcmp(env, "false") &&
	       strcmp(env, "FALSE") && strcmp(env, "no") && strcmp(env, "NO");
}

static inline int
hostmem_config_uses_hugepages(const struct hostmem_config *config)
{
	return config && config->use_hugepages;
}

static inline int
hostmem_config_init(struct hostmem_config *config)
{
	const char *env;
	int err;

	snprintf(config->hugetlb_path, sizeof(config->hugetlb_path), "/mnt/huge");
	config->pagesize = getpagesize();
	config->pagesize_shift = upcie_util_shift_from_size(config->pagesize);
	config->use_hugepages = 1;

	env = getenv("HOSTMEM_NO_HUGEPAGE");
	if (hostmem_env_is_true(env)) {
		config->use_hugepages = 0;
	}

	if (config->use_hugepages) {
		err = hostmem_config_get_hugepgsz(&config->hugepgsz);
		if (err) {
			return err;
		}

		config->memfd_flags = MFD_HUGETLB;
		if (config->hugepgsz == 2 * 1024 * 1024) {
			config->memfd_flags |= MFD_HUGE_2MB;
		} else if (config->hugepgsz == 1 * 1024 * 1024 * 1024) {
			config->memfd_flags |= MFD_HUGE_1GB;
		} else {
			fprintf(stderr, "Unsupported hugepgsz(%d)\n", config->hugepgsz);
			return -EINVAL;
		}
	} else {
		config->hugepgsz = config->pagesize;
		config->memfd_flags = 0;
	}

	env = getenv("HOSTMEM_HUGETLB_PATH");
	if (env) {
		snprintf(config->hugetlb_path, sizeof(config->hugetlb_path), "%s", env);
	}

	env = getenv("HOSTMEM_BACKEND");
	if (env) {
		if (env && strcmp(env, "memfd") == 0) {
			config->backend = HOSTMEM_BACKEND_MEMFD;
		} else if (env && strcmp(env, "hugetlbfs") == 0) {
			config->backend = HOSTMEM_BACKEND_HUGETLBFS;
		} else {
			return -EINVAL;
		}
	} else {
		config->backend = HOSTMEM_BACKEND_MEMFD;
	}

	if (!config->use_hugepages && config->backend == HOSTMEM_BACKEND_HUGETLBFS) {
		fprintf(stderr, "HOSTMEM_BACKEND=hugetlbfs requires hugepages\n");
		return -EINVAL;
	}

	return 0;
}
