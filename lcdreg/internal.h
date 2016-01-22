
#ifdef CONFIG_DEBUG_FS
void lcdreg_debugfs_init(struct lcdreg *reg);
void lcdreg_debugfs_exit(struct lcdreg *reg);
#else
static inline void lcdreg_debugfs_init(struct lcdreg *reg) { }
static inline void lcdreg_debugfs_exit(struct lcdreg *reg) { }
#endif
