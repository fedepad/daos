/**
 * (C) Copyright 2019-2021 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * DAOS Unified namespace functionality.
 */

#define D_LOGFAC	DD_FAC(duns)

#include <dirent.h>
#include <libgen.h>
#include <sys/xattr.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include <regex.h>
#include <pwd.h>
#ifdef LUSTRE_INCLUDE
#include <lustre/lustreapi.h>
#include <linux/lustre/lustre_idl.h>
#endif
#include <daos/common.h>
#include <daos/object.h>
#include "daos_types.h"
#include "daos.h"
#include "daos_fs.h"
#include "daos_uns.h"

#define DUNS_XATTR_FMT		"DAOS.%s://%36s/%36s"

#ifndef FUSE_SUPER_MAGIC
#define FUSE_SUPER_MAGIC	0x65735546
#endif

#ifdef LUSTRE_INCLUDE
#define LIBLUSTRE		"liblustreapi.so"

static bool liblustre_notfound;
/* need to protect against concurrent/multi-threaded attempts to bind ? */
static bool liblustre_binded;
static int (*dir_create_foreign)(const char *, mode_t, __u32, __u32,
				 const char *) = NULL;
static int (*unlink_foreign)(char *);

static int
bind_liblustre()
{
	void *lib;

	/* bind to lustre lib/API */
	lib = dlopen(LIBLUSTRE, RTLD_NOW);
	if (lib == NULL) {
		liblustre_notfound = true;
		D_ERROR("unable to locate/bind %s, dlerror() says '%s', "
			"reverting to non-lustre behavior.\n",
			LIBLUSTRE, dlerror());
		return EINVAL;
	}

	D_DEBUG(DB_TRACE, "%s has been found and dynamically binded !\n",
		LIBLUSTRE);

	/* now try to map the API methods we need */
	dir_create_foreign = (int (*)(const char *, mode_t, __u32, __u32,
			      const char *))dlsym(lib,
						  "llapi_dir_create_foreign");
	if (dir_create_foreign == NULL) {
		liblustre_notfound = true;
		D_ERROR("unable to resolve llapi_dir_create_foreign symbol, "
			"dlerror() says '%s', Lustre version do not seem to "
			"support foreign LOV/LMV, reverting to non-lustre "
			"behavior.\n", dlerror());
		return EINVAL;
	}

	D_DEBUG(DB_TRACE, "llapi_dir_create_foreign() resolved at %p\n",
		dir_create_foreign);

	unlink_foreign = (int (*)(char *))dlsym(lib,
						      "llapi_unlink_foreign");
	if (unlink_foreign == NULL) {
		liblustre_notfound = true;
		D_ERROR("unable to resolve llapi_unlink_foreign symbol, "
			"dlerror() says '%s', Lustre version do not seem to "
			"support foreign daos type, reverting to non-lustre "
			"behavior.\n", dlerror());
		return EINVAL;
	}

	D_DEBUG(DB_TRACE, "llapi_unlink_foreign() resolved at %p\n",
		unlink_foreign);

	liblustre_binded = true;
	return 0;
}

static int
duns_resolve_lustre_path(const char *path, struct duns_attr_t *attr)
{
	char			str[DUNS_MAX_XATTR_LEN + 1];
	char			*saveptr, *t;
	char			*buf;
	struct lmv_user_md	*lum;
	struct lmv_foreign_md	*lfm;
	int			fd;
	int			rc;

	/* XXX if a Posix container is not always mapped with a daos foreign dir
	 * with LMV, both LOV and LMV will need to be queried if ENODATA is
	 * returned at 1st, as the file/dir type is hidden to help decide before
	 * due to the symlink fake !!
	 */

	/* XXX if liblustreapi is not binded, do it now ! */
	if (liblustre_binded == false && liblustre_notfound == false) {
		rc = bind_liblustre();
		if (rc)
			return rc;
	} else if (liblustre_notfound == true) {
		/* no liblustreapi.so found, or incompatible */
		return EINVAL;
	} else if (liblustre_binded == false) {
		/* this should not happen */
		D_ERROR("liblustre_notfound == false && liblustre_notfound == "
			"false not expected after bind_liblustre()\n");
		return EINVAL;
	}

	D_DEBUG(DB_TRACE, "Trying to retrieve associated container's infos "
		"from Lustre path '%s'\n", path);

	/* get LOV/LMV
	 * llapi_getstripe() API can not be used here as it frees
	 * param.[fp_lmv_md, fp_lmd->lmd_lmm]  buffer for LMV after printing
	 * its content
	 * raw/ioctl() way must be used then
	 */
	buf = calloc(1, XATTR_SIZE_MAX);
	if (buf == NULL) {
		D_ERROR("unable to allocate XATTR_SIZE_MAX to get LOV/LMV "
			"for '%s', errno %d(%s).\n", path, errno,
			strerror(errno));
		return ENOMEM;
	}
	fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (fd == -1 && errno != ENOTDIR) {
		int err = errno;

		D_ERROR("unable to open '%s' errno %d(%s).\n", path, errno,
			strerror(errno));
		return err;
	} else if (errno == ENOTDIR) {
		/* should we handle file/LOV case ?
		 * for link to HDF5 container ?
		 */
		D_ERROR("file with foreign LOV support is presently not "
			"supported\n");
	} else {
		/* it is a dir so get LMV ! */
		lum = (struct lmv_user_md *)buf;
		/* to get LMV and not default one !! */
		lum->lum_magic = LMV_MAGIC_V1;
		/* to confirm we have already a buffer large enough get a
		 * BIG LMV !!
		 */
		lum->lum_stripe_count = (XATTR_SIZE_MAX -
					sizeof(struct lmv_user_md)) /
					sizeof(struct lmv_user_mds_data);
		rc = ioctl(fd, LL_IOC_LMV_GETSTRIPE, buf);
		if (rc != 0) {
			int err = errno;

			D_ERROR("ioctl(LL_IOC_LMV_GETSTRIPE) failed, rc: %d, "
				"errno %d(%s).\n", rc, errno, strerror(errno));
			return err;
		}

		lfm = (struct lmv_foreign_md *)buf;
		/* sanity check */
		if (lfm->lfm_magic != LMV_MAGIC_FOREIGN  ||
		    lfm->lfm_type != LU_FOREIGN_TYPE_SYMLINK ||
		    lfm->lfm_length > DUNS_MAX_XATTR_LEN ||
		    snprintf(str, DUNS_MAX_XATTR_LEN, "%s",
			     lfm->lfm_value) > DUNS_MAX_XATTR_LEN) {
			D_ERROR("Invalid DAOS LMV format (%s).\n", str);
			return EINVAL;
		}
	}

	t = strtok_r(str, ".", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS LMV format (%s).\n", str);
		return EINVAL;
	}

	t = strtok_r(NULL, ":", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS LMV format (%s).\n", str);
		return EINVAL;
	}

	daos_parse_ctype(t, &attr->da_type);
	if (attr->da_type == DAOS_PROP_CO_LAYOUT_UNKOWN) {
		D_ERROR("Invalid DAOS LMV format: Container layout cannot be"
			" unknown\n");
		return EINVAL;
	}

	t = strtok_r(NULL, "/", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS LMV format (%s).\n", str);
		return EINVAL;
	}

	rc = uuid_parse(t, attr->da_puuid);
	if (rc) {
		D_ERROR("Invalid DAOS LMV format: pool UUID cannot be"
			" parsed\n");
		return EINVAL;
	}

	t = strtok_r(NULL, "/", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS LMV format (%s).\n", str);
		return EINVAL;
	}

	rc = uuid_parse(t, attr->da_cuuid);
	if (rc) {
		D_ERROR("Invalid DAOS LMV format: container UUID cannot be"
			" parsed\n");
		return EINVAL;
	}

	/* path is DAOS-foreign and will need to be unlinked using
	 * unlink_foreign API
	 */
	attr->da_on_lustre = true;

	return 0;
}
#endif

#define UUID_REGEX "([a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}){1}"
#define DAOS_FORMAT "^daos://"UUID_REGEX"/"UUID_REGEX"[/]?"
#define DAOS_FORMAT_NO_PREFIX "^/"UUID_REGEX"/"UUID_REGEX"[/]?"
#define DAOS_FORMAT_NO_CONT "^daos://"UUID_REGEX"[/]?$"

static int
check_direct_format(const char *path, bool no_prefix, bool *pool_only)
{
	regex_t regx;
	int	rc;

	if (no_prefix)
		rc = regcomp(&regx, DAOS_FORMAT_NO_PREFIX,
			     REG_EXTENDED | REG_ICASE);
	else
		rc = regcomp(&regx, DAOS_FORMAT, REG_EXTENDED | REG_ICASE);
	if (rc)
		return EINVAL;

	rc = regexec(&regx, path, 0, NULL, 0);
	regfree(&regx);
	if (rc == 0) {
		*pool_only = false;
		return rc;
	} else if (rc != REG_NOMATCH) {
		return rc;
	}

	rc = regcomp(&regx, DAOS_FORMAT_NO_CONT, REG_EXTENDED | REG_ICASE);
	if (rc)
		return EINVAL;

	rc = regexec(&regx, path, 0, NULL, 0);
	if (rc == 0)
		*pool_only = true;

	regfree(&regx);
	return rc;
}

int
duns_resolve_path(const char *path, struct duns_attr_t *attr)
{
	ssize_t		s;
	char		str[DUNS_MAX_XATTR_LEN];
	struct statfs	fs;
	bool		pool_only = false;
	int		rc;

	rc = check_direct_format(path, attr->da_no_prefix, &pool_only);
	if (rc == 0) {
		char	*dir;
		char	*saveptr, *t;

		D_STRNDUP(dir, path, PATH_MAX);
		if (dir == NULL) {
			D_ERROR("Failed to copy path\n");
			return ENOMEM;
		}

		D_DEBUG(DB_TRACE, "DUNS resolve to direct path: %s\n", dir);
		t = strtok_r(dir, "/", &saveptr);
		if (t == NULL) {
			D_ERROR("Invalid DAOS format (%s).\n", path);
			D_FREE(dir);
			return EINVAL;
		}

		/** if there is a daos: prefix, skip over it */
		if (!attr->da_no_prefix) {
			t = strtok_r(NULL, "/", &saveptr);
			if (t == NULL) {
				D_ERROR("Invalid DAOS format (%s).\n", path);
				D_FREE(dir);
				return EINVAL;
			}
		}

		/** parse the pool uuid */
		rc = uuid_parse(t, attr->da_puuid);
		if (rc) {
			D_ERROR("Invalid format: pool UUID cannot be parsed\n");
			D_FREE(dir);
			return EINVAL;
		}

		if (pool_only) {
			D_FREE(dir);
			return 0;
		}

		t = strtok_r(NULL, "/", &saveptr);
		if (t == NULL) {
			D_ERROR("Invalid DAOS format (%s).\n", path);
			D_FREE(dir);
			return EINVAL;
		}

		/** parse the container uuid */
		rc = uuid_parse(t, attr->da_cuuid);
		if (rc) {
			D_ERROR("Invalid format: cont UUID cannot be parsed\n");
			D_FREE(dir);
			return EINVAL;
		}

		/** if there is a relative path, parse it out */
		t = strtok_r(NULL, "", &saveptr);
		if (t != NULL) {
			D_ASPRINTF(attr->da_rel_path, "/%s", t);
			if (!attr->da_rel_path) {
				D_FREE(dir);
				return ENOMEM;
			}
		}

		D_FREE(dir);
		return 0;
	}

	rc = statfs(path, &fs);
	if (rc == -1) {
		int err = errno;

		D_INFO("Failed to statfs %s: %s\n", path, strerror(errno));
		return err;
	}

#ifdef LUSTRE_INCLUDE
	if (fs.f_type == LL_SUPER_MAGIC) {
		rc = duns_resolve_lustre_path(path, attr);
		if (rc == 0)
			return 0;

		/* if Lustre specific method fails, fallback to try
		 * the normal way...
		 */
	}
#endif

	s = lgetxattr(path, DUNS_XATTR_NAME, &str, DUNS_MAX_XATTR_LEN);
	if (s < 0 || s > DUNS_MAX_XATTR_LEN) {
		int err = errno;

		if (err == ENOTSUP) {
			D_INFO("Path is not in a filesystem that supports the"
				" DAOS unified namespace\n");
		} else if (err == ENODATA) {
			D_INFO("Path does not represent a DAOS link\n");
		} else if (s > DUNS_MAX_XATTR_LEN) {
			err = EIO;
			D_ERROR("Invalid xattr length\n");
		} else {
			D_ERROR("Invalid DAOS unified namespace xattr: %s\n",
				strerror(err));
		}

		return err;
	}

	return duns_parse_attr(&str[0], s, attr);
}

int
duns_parse_attr(char *str, daos_size_t len, struct duns_attr_t *attr)
{
	char	*local;
	char	*saveptr = NULL, *t;
	int	rc;

	D_STRNDUP(local, str, len);
	if (!local)
		return ENOMEM;

	t = strtok_r(local, ".", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS xattr format (%s).\n", str);
		D_GOTO(err, rc = EINVAL);
	}

	t = strtok_r(NULL, ":", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS xattr format (%s).\n", str);
		D_GOTO(err, rc = EINVAL);
	}
	daos_parse_ctype(t, &attr->da_type);
	if (attr->da_type == DAOS_PROP_CO_LAYOUT_UNKOWN) {
		D_ERROR("Invalid DAOS xattr format: Container layout cannot be"
			" unknown\n");
		D_GOTO(err, rc = EINVAL);
	}

	t = strtok_r(NULL, "/", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS xattr format (%s).\n", str);
		D_GOTO(err, rc = EINVAL);
	}

	rc = uuid_parse(t, attr->da_puuid);
	if (rc) {
		D_ERROR("Invalid DAOS xattr format: pool UUID cannot be"
			" parsed\n");
		D_GOTO(err, rc = EINVAL);
	}

	t = strtok_r(NULL, "/", &saveptr);
	if (t == NULL) {
		D_ERROR("Invalid DAOS xattr format (%s).\n", str);
		D_GOTO(err, rc = EINVAL);
	}
	rc = uuid_parse(t, attr->da_cuuid);
	if (rc) {
		D_ERROR("Invalid DAOS xattr format: container UUID cannot be"
			" parsed\n");
		D_GOTO(err, rc = EINVAL);
	}

	rc = 0;
err:
	D_FREE(local);
	return rc;
}

#ifdef LUSTRE_INCLUDE
static int
duns_create_lustre_path(daos_handle_t poh, const char *path,
			struct duns_attr_t *attrp)
{
	char			pool[37], cont[37];
	char			oclass[10], type[10];
	char			str[DUNS_MAX_XATTR_LEN + 1];
	int			len;
	int			try_multiple = 1;		/* boolean */
	int			rc;

	/* XXX pool must already be created, and associated DFuse-mount
	 * should already be mounted
	 */

	/* XXX if liblustreapi is not binded, do it now ! */
	if (liblustre_binded == false && liblustre_notfound == false) {
		rc = bind_liblustre();
		if (rc)
			return EINVAL;
	}

	uuid_unparse(attrp->da_puuid, pool);
	daos_oclass_id2name(attrp->da_oclass_id, oclass);
	daos_unparse_ctype(attrp->da_type, type);

	/* create container with specified container uuid (try_multiple=0)
	 * or a generated random container uuid (try_multiple!=0).
	 */
	if (!uuid_is_null(attrp->da_cuuid)) {
		try_multiple = 0;
		uuid_unparse(attrp->da_cuuid, cont);
		D_INFO("try create once with provided container UUID: %36s\n",
		       cont);
	}
	/* create container */
	do {
		if (try_multiple) {
			uuid_generate(attrp->da_cuuid);
			uuid_unparse(attrp->da_cuuid, cont);
		}

		if (attrp->da_type == DAOS_PROP_CO_LAYOUT_POSIX) {
			dfs_attr_t	dfs_attr = { 0 };

			/** TODO: set Lustre FID here. */
			dfs_attr.da_id = 0;
			dfs_attr.da_oclass_id = attrp->da_oclass_id;
			dfs_attr.da_chunk_size = attrp->da_chunk_size;
			dfs_attr.da_props = attrp->da_props;
			rc = dfs_cont_create(poh, attrp->da_cuuid, &dfs_attr,
					     NULL, NULL);
		} else {
			daos_prop_t	*prop;
			int		 nr = 1;

			if (attrp->da_props != NULL)
				nr = attrp->da_props->dpp_nr + 1;

			prop = daos_prop_alloc(nr);
			if (prop == NULL) {
				D_ERROR("Failed to allocate container prop.");
				D_GOTO(err, rc = ENOMEM);
			}
			if (attrp->da_props != NULL) {
				rc = daos_prop_copy(prop, attrp->da_props);
				if (rc) {
					daos_prop_free(prop);
					D_ERROR("failed to copy properties (%d)\n", rc);
					return daos_der2errno(rc);
				}
			}
			prop->dpp_entries[prop->dpp_nr - 1].dpe_type =
				DAOS_PROP_CO_LAYOUT_TYPE;
			prop->dpp_entries[prop->dpp_nr - 1].dpe_val =
				attrp->da_type;
			rc = daos_cont_create(poh, attrp->da_cuuid, prop, NULL);
			daos_prop_free(prop);
		}

	} while ((rc == -DER_EXIST) && try_multiple);

	if (rc) {
		D_ERROR("Failed to create container (%d)\n", rc);
		D_GOTO(err, rc);
	}

	/* XXX should file with foreign LOV be expected/supoorted here ? */

	/** create dir and store the daos attributes in the path LMV */
	len = snprintf(str,
		       DUNS_MAX_XATTR_LEN, DUNS_XATTR_FMT, type, pool, cont);
	if (len < 0) {
		D_ERROR("Failed to create LMV value\n");
		D_GOTO(err_cont, rc = EINVAL);
	}

	rc = (*dir_create_foreign)(path, S_IRWXU | S_IRWXG | S_IROTH | S_IWOTH,
				   LU_FOREIGN_TYPE_SYMLINK, 0xda05, str);
	if (rc) {
		D_ERROR("Failed to create Lustre dir '%s' with foreign "
			"LMV '%s' (rc = %d).\n", path, str, rc);
		D_GOTO(err_cont, rc = EINVAL);
	}

	return rc;

err_cont:
	daos_cont_destroy(poh, attrp->da_cuuid, 1, NULL);
err:
	return rc;
}
#endif

#define PW_BUF_SIZE 1024

static int
duns_set_fuse_acl(const char *path, daos_handle_t coh)
{
	char		*buf;
	int		rc = 0;
	struct daos_acl	*acl;
	struct daos_ace	*ace;
	struct stat	stbuf = {};
	int		uid;
	struct passwd	pwd = {};
	struct passwd	*pwdp = NULL;
	char		*name;

	rc = stat(path, &stbuf);
	if (rc == -1)
		return errno;

	uid = geteuid();

	if (uid == stbuf.st_uid) {
		D_DEBUG(DB_TRACE, "Same user, returning\n");
		return 0;
	}

	printf("Setting ACL for new container\n");

	/* TODO: Use daos_acl_uid_to_principal() here */

	D_ALLOC(buf, PW_BUF_SIZE);
	if (buf == NULL)
		return ENOMEM;

	errno = 0;
	rc = getpwuid_r(stbuf.st_uid, &pwd, buf, PW_BUF_SIZE, &pwdp);
	if (rc == -1 || pwdp == NULL) {
		int err = errno;

		D_ERROR("getpwuid() failed, (%s)\n", strerror(rc));
		D_GOTO(out_buf, rc = err);
	}

	D_ASPRINTF(name, "%s@", pwdp->pw_name);
	if (name == NULL)
		D_GOTO(out_buf, rc = ENOMEM);

	ace = daos_ace_create(DAOS_ACL_USER, name);
	if (ace == NULL) {
		D_ERROR("daos_ace_create() failed.\n");
		D_GOTO(out_name, rc = EIO);
	}

	ace->dae_access_types = DAOS_ACL_ACCESS_ALLOW;
	ace->dae_allow_perms = DAOS_ACL_PERM_READ | DAOS_ACL_PERM_WRITE |
		DAOS_ACL_PERM_GET_PROP | DAOS_ACL_PERM_GET_ACL;

	acl = daos_acl_create(&ace, 1);
	if (acl == NULL)
		D_GOTO(out_ace, rc = EIO);

	rc = daos_cont_update_acl(coh, acl, NULL);
	if (rc) {
		D_ERROR("daos_cont_update_acl() failed, " DF_RC "\n",
			DP_RC(rc));
	}

	daos_acl_free(acl);

out_ace:
	daos_ace_free(ace);
out_name:
	D_FREE(name);
out_buf:
	D_FREE(buf);
	return rc;
}

static int
create_cont(daos_handle_t poh, struct duns_attr_t *attrp,
	    const char *path, bool backend_dfuse)
{
	int rc;

	if (attrp->da_type == DAOS_PROP_CO_LAYOUT_POSIX) {
		dfs_attr_t dfs_attr = {};
		daos_handle_t   coh;

		/** TODO: set Lustre FID here. */
		dfs_attr.da_id = 0;
		dfs_attr.da_oclass_id = attrp->da_oclass_id;
		dfs_attr.da_chunk_size = attrp->da_chunk_size;
		dfs_attr.da_props = attrp->da_props;
		rc = dfs_cont_create(poh, attrp->da_cuuid, &dfs_attr,
				     backend_dfuse ? &coh : NULL, NULL);
		if (rc == -DER_SUCCESS && backend_dfuse) {
			rc = duns_set_fuse_acl(path, coh);
			daos_cont_close(coh, NULL);
		}

	} else {
		daos_prop_t	*prop;
		int		 nr = 1;

		if (attrp->da_props != NULL)
			nr = attrp->da_props->dpp_nr + 1;

		prop = daos_prop_alloc(nr);
		if (prop == NULL) {
			D_ERROR("Failed to allocate container prop.");
			return ENOMEM;
		}
		if (attrp->da_props != NULL) {
			rc = daos_prop_copy(prop, attrp->da_props);
			if (rc) {
				daos_prop_free(prop);
				D_ERROR("failed to copy properties (%d)\n", rc);
				return daos_der2errno(rc);
			}
		}
		prop->dpp_entries[prop->dpp_nr - 1].dpe_type =
			DAOS_PROP_CO_LAYOUT_TYPE;
		prop->dpp_entries[prop->dpp_nr - 1].dpe_val = attrp->da_type;
		rc = daos_cont_create(poh, attrp->da_cuuid, prop, NULL);
		if (rc)
			rc = daos_der2errno(rc);
		daos_prop_free(prop);
	}

	return rc;
}

int
duns_create_path(daos_handle_t poh, const char *path, struct duns_attr_t *attrp)
{
	char		pool[37], cont[37];
	char		oclass[10], type[10];
	char		str[DUNS_MAX_XATTR_LEN];
	int		len;
	bool		try_multiple = true;
	int		rc;
	bool		backend_dfuse = false;
	bool		pool_only;
	size_t		path_len;

	if (path == NULL) {
		D_ERROR("Invalid path\n");
		return EINVAL;
	}

	path_len = strlen(path);

	rc = check_direct_format(path, attrp->da_no_prefix, &pool_only);
	if (rc == 0) {
		if (pool_only) {
			D_ERROR("Invalid DUNS format: %s\n", path);
			return EINVAL;
		}

		rc = create_cont(poh, attrp, NULL, false);
		if (rc)
			D_ERROR("Failed to create container (%d)\n", rc);
		return rc;
	}

	if (attrp->da_type == DAOS_PROP_CO_LAYOUT_HDF5) {
		/** create a new file if HDF5 container */
		int fd;

		fd = open(path, O_CREAT | O_EXCL,
			  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
		if (fd == -1) {
			rc = errno;

			D_ERROR("Failed to create file %s: %s\n", path,
				strerror(rc));
			return rc;
		}
		close(fd);
	} else if (attrp->da_type == DAOS_PROP_CO_LAYOUT_POSIX) {
		struct statfs   fs;
		char            *dir, *dirp;
		mode_t		mode = S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH;

		D_STRNDUP(dir, path, path_len);
		if (dir == NULL) {
			D_ERROR("Failed copy path %s: %s\n", path,
				strerror(errno));
			return ENOMEM;
		}

		/* dirname() may modify dir content or not, so use an
		 * alternate pointer (see dirname() man page)
		 */
		dirp = dirname(dir);
		rc = statfs(dirp, &fs);
		if (rc == -1) {
			int err = errno;

			D_ERROR("Failed to statfs dir %s: %s\n",
				dirp, strerror(errno));
			D_FREE(dir);
			return err;
		}
		D_FREE(dir);

		if (fs.f_type == FUSE_SUPER_MAGIC)
			backend_dfuse = true;

#ifdef LUSTRE_INCLUDE
		if (fs.f_type == LL_SUPER_MAGIC) {
			rc = duns_create_lustre_path(poh, path, attrp);
			if (rc == 0)
				return 0;
			/* if Lustre specific method fails, fallback to try
			 * the normal way...
			 */
		}
#endif

		/** create a new directory if POSIX/MPI-IO container */
		rc = mkdir(path, mode);
		if (rc == -1) {
			rc = errno;

			D_ERROR("Failed to create dir %s: %s\n",
				path, strerror(rc));
			return rc;
		}
	} else {
		D_ERROR("Invalid container layout.\n");
		return EINVAL;
	}

	uuid_unparse(attrp->da_puuid, pool);
	if (attrp->da_oclass_id != OC_UNKNOWN)
		daos_oclass_id2name(attrp->da_oclass_id, oclass);
	else
		strcpy(oclass, "UNKNOWN");
	daos_unparse_ctype(attrp->da_type, type);

	/* create container with specified container uuid (try_multiple)
	 * or a generated random container uuid (!try_multiple).
	 */
	if (!uuid_is_null(attrp->da_cuuid)) {
		try_multiple = false;
		uuid_unparse(attrp->da_cuuid, cont);
		D_INFO("try create once with provided container UUID: %36s\n",
		       cont);
	}
	do {
		if (try_multiple) {
			uuid_generate(attrp->da_cuuid);
			uuid_unparse(attrp->da_cuuid, cont);
		}

		/** store the daos attributes in the path xattr */
		len = snprintf(str,
			       DUNS_MAX_XATTR_LEN,
			       DUNS_XATTR_FMT,
			       type,
			       pool,
			       cont);
		if (len < 0) {
			D_ERROR("Failed to create xattr value\n");
			D_GOTO(err_link, rc = EINVAL);
		}

		rc = lsetxattr(path, DUNS_XATTR_NAME, str, len + 1, 0);
		if (rc) {
			rc = errno;
			if (rc == ENOTSUP) {
				D_INFO("Path is not in a filesystem that "
					"supports the DAOS unified "
					"namespace\n");
			} else {
				D_ERROR("Failed to set DAOS xattr: %s\n",
					strerror(rc));
			}
			goto err_link;
		}

		rc = create_cont(poh, attrp, path, true);
		if (rc == -DER_SUCCESS && backend_dfuse) {
			/* This next setxattr will cause dfuse to lookup the
			 * entry point and perform a container connect,
			 * therefore this xattr will be set in the root of the
			 * new container, not the directory.
			 */
			rc = lsetxattr(path, DUNS_XATTR_NAME, str,
				       len + 1, XATTR_CREATE);
			if (rc) {
				rc = errno;
				D_ERROR("Failed to set DAOS xattr: %s\n",
					strerror(rc));
				goto err_link;
			}
		}

	} while ((rc == -DER_EXIST) && try_multiple);
	if (rc) {
		D_ERROR("Failed to create container (%d)\n", rc);
		D_GOTO(err_link, rc = daos_der2errno(rc));
	}

	return rc;
err_link:
	if (attrp->da_type == DAOS_PROP_CO_LAYOUT_HDF5)
		unlink(path);
	else if (attrp->da_type == DAOS_PROP_CO_LAYOUT_POSIX)
		rmdir(path);
	return rc;
}

int
duns_destroy_path(daos_handle_t poh, const char *path)
{
	struct duns_attr_t dattr = {0};
	int	rc;

	/* Resolve pool, container UUIDs from path */
	rc = duns_resolve_path(path, &dattr);
	if (rc) {
		D_ERROR("duns_resolve_path() Failed on path %s (%d)\n",
			path, rc);
		return rc;
	}

	/** Destroy the container */
	rc = daos_cont_destroy(poh, dattr.da_cuuid, 1, NULL);
	if (rc) {
		D_ERROR("Failed to destroy container (%d)\n", rc);
		/** recreate the link ? */
		return daos_der2errno(rc);
	}

	if (dattr.da_type == DAOS_PROP_CO_LAYOUT_HDF5) {
#ifdef LUSTRE_INCLUDE
		if (dattr.da_on_lustre)
			rc = (*unlink_foreign)((char *)path);
		else
#endif
			rc = unlink(path);
		if (rc) {
			int err = errno;

			D_ERROR("Failed to unlink %sfile %s: %s\n",
				dattr.da_on_lustre ? "Lustre " : " ", path,
				strerror(errno));
			return err;
		}
	} else if (dattr.da_type == DAOS_PROP_CO_LAYOUT_POSIX) {
#ifdef LUSTRE_INCLUDE
		if (dattr.da_on_lustre)
			rc = (*unlink_foreign)((char *)path);
		else
#endif
			rc = rmdir(path);
		if (rc) {
			int err = errno;

			D_ERROR("Failed to remove %sdir %s: %s\n",
				dattr.da_on_lustre ? "Lustre " : " ", path,
				strerror(errno));
			return err;
		}
	}

	return 0;
}
