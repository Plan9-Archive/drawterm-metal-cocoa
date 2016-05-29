extern int havesecstore(char *addr, char *owner);
extern char *secstore;
extern char secstorebuf[65536];
extern char *secstorefetch(char *addr, char *owner, char *passwd);
extern char *authserver;
extern char *readcons(char *prompt, char *def, int secret);
extern int exportfs(int);
extern char *user;
extern int dialfactotum(void);
extern char *getuser(void);
extern void cpumain(int, char**);
extern char *estrdup(char*);
extern int aanclient(char*);

