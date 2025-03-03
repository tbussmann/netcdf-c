/*! \file

Copyright 2018 University Corporation for Atmospheric
Research/Unidata. See \ref copyright file for more info.  */

#include "config.h"
#include <stdio.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#if defined(_WIN32) && !defined(__MINGW32__)
#include "XGetopt.h"
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <math.h>

#include "netcdf.h"
#include "netcdf_mem.h"
#include "netcdf_filter.h"
#include "netcdf_aux.h"
#include "utils.h"
#include "nccomps.h"
#include "nctime0.h"		/* new iso time and calendar stuff */
#include "dumplib.h"
#include "ncdump.h"
#include "vardata.h"
#include "indent.h"
#include "isnan.h"
#include "cdl.h"
#include "nclog.h"
#include "ncpathmgr.h"
#include "nclist.h"
#include "ncuri.h"
#include "nc_provenance.h"
#include "ncpathmgr.h"

#ifdef USE_NETCDF4
#include "nc4internal.h" /* to get name of the special properties file */
#endif

#define XML_VERSION "1.0"

#define LPAREN "("
#define RPAREN ")"

#define int64_t long long
#define uint64_t unsigned long long

/* If we have a variable named one of these:
   we need to be careful about printing their attributes.
*/
static const char* keywords[] = {
"variables",
"dimensions",
"data",
"group",
"types",
NULL
};

/*Forward*/
static int searchgrouptreedim(int ncid, int dimid, int* parentidp);
extern int nc__testurl(const char*,char**);

static int iskeyword(const char* kw)
{
    const char** p;
    for(p=keywords;*p;p++) {
	if(strcmp(kw,*p)==0) return 1;
    }
    return 0;
}

/* globals */
char *progname;
fspec_t formatting_specs =	/* defaults, overridden by command-line options */
{
    0,			/* construct netcdf name from file name */
    false,		/* print header info only, no data? */
    false,		/* just print coord vars? */
    false,		/* brief  comments in data section? */
    false,		/* full annotations in data section?  */
    false,		/* human-readable output for date-time values? */
    false,		/* use 'T' separator between date and time values as strings? */
    false,		/* output special attributes, eg chunking? */
    false,              /* if -F specified */
    LANG_C,		/* language conventions for indices */
    false,	        /* for DAP URLs, client-side cache used */
    0,			/* if -v specified, number of variables in list */
    0,			/* if -v specified, list of variable names */
    0,			/* if -g specified, number of groups names in list */
    0,			/* if -g specified, list of group names */
    0,			/* if -g specified, list of matching grpids */
    0,			/* kind of netCDF file */
    0,                  /* extended format */
    0,                  /* mode */
    0,                  /* inmemory */
    0,                  /* print _NCproperties */
    0                   /* print _FillValue type*/
};

static void
usage(void)
{
#define USAGE   "\
  [-c]             Coordinate variable data and header information\n\
  [-h]             Header information only, no data\n\
  [-v var1[,...]]  Data for variable(s) <var1>,... only\n\
  [-b [c|f]]       Brief annotations for C or Fortran indices in data\n\
  [-f [c|f]]       Full annotations for C or Fortran indices in data\n\
  [-l len]         Line length maximum in data section (default 80)\n\
  [-n name]        Name for netCDF (default derived from file name)\n\
  [-p n[,n]]       Display floating-point values with less precision\n\
  [-k]             Output kind of netCDF file\n\
  [-s]             Output special (virtual) attributes\n\
  [-t]             Output time data as date-time strings\n\
  [-i]             Output time data as date-time strings with ISO-8601 'T' separator\n\
  [-g grp1[,...]]  Data and metadata for group(s) <grp1>,... only\n\
  [-w]             With client-side caching of variables for DAP URLs\n\
  [-x]             Output XML (NcML) instead of CDL\n\
  [-F]             Output _Filter and _Codecs instead of _Fletcher32, _Shuffle, and _Deflate\n\
  [-Xp]            Unconditionally suppress output of the properties attribute\n\
  [-XF]            Unconditionally output the type of the _FillValue attribute\n\
  [-Ln]            Set log level to n (>= 0); ignore if logging not enabled.\n\
  file             Name of netCDF file (or URL if DAP access enabled)\n"

    (void) fprintf(stderr,
		   "%s [-c|-h] [-v ...] [[-b|-f] [c|f]] [-l len] [-n name] [-p n[,n]] [-k] [-x] [-s] [-t|-i] [-g ...] [-w] [-F] [-Ln] file\n%s",
		   progname,
		   USAGE);

    (void) fprintf(stderr,
                 "netcdf library version %s\n",
                 nc_inq_libvers());
}


/*
 * convert pathname of netcdf file into name for cdl unit, by taking
 * last component of path and stripping off any extension.
 * DMH: add code to handle OPeNDAP url.
 * DMH: I think this also works for UTF8.
 */
static char *
name_path(const char *path)
{
    char* cvtpath = NULL;
    const char *cp = NULL;
    char *sp = NULL;
    size_t cplen = 0;
    char* base = NULL;

    if (NCpathcanonical(path, &cvtpath) || cvtpath==NULL)
        return NULL;

    /* See if this is a url */
    if(nc__testurl(cvtpath,&base))
 	 goto done; /* Looks like a url */
    /* else fall thru and treat like a file path */

    cp = strrchr(cvtpath, '/');
    if (cp == NULL)		/* no delimiter */
      cp = cvtpath;
    else			/* skip delimiter */
      cp++;
    cplen = strlen(cp);
    base = (char *) emalloc((unsigned) (cplen+1));
    base[0] = '\0';
    strlcat(base,cp,cplen+1);
    if ((sp = strrchr(base, '.')) != NULL)
      *sp = '\0';		/* strip off any extension */

done:
    nullfree(cvtpath);
    return base;
}

/* Return primitive type name */
static const char *
prim_type_name(nc_type type)
{
    switch (type) {
      case NC_BYTE:
	return "byte";
      case NC_CHAR:
	return "char";
      case NC_SHORT:
	return "short";
      case NC_INT:
	return "int";
      case NC_FLOAT:
	return "float";
      case NC_DOUBLE:
	return "double";
      case NC_UBYTE:
	return "ubyte";
      case NC_USHORT:
	return "ushort";
      case NC_UINT:
	return "uint";
      case NC_INT64:
	return "int64";
      case NC_UINT64:
	return "uint64";
      case NC_STRING:
	return "string";
      case NC_VLEN:
	return "vlen";
      case NC_OPAQUE:
	return "opaque";
      case NC_COMPOUND:
	return "compound";
      default:
	error("prim_type_name: bad type %d", type);
	return "bogus";
    }
}


/*
 * Remove trailing zeros (after decimal point) but not trailing decimal
 * point from ss, a string representation of a floating-point number that
 * might include an exponent part.
 */
static void
tztrim(char *ss)
{
    char *cp, *ep;

    cp = ss;
    if (*cp == '-')
      cp++;
    while(isdigit((int)*cp) || *cp == '.')
      cp++;
    if (*--cp == '.')
      return;
    ep = cp+1;
    while (*cp == '0')
      cp--;
    cp++;
    if (cp == ep)
      return;
    while (*ep)
      *cp++ = *ep++;
    *cp = '\0';
    return;
}


/* Return file type string */
static const char *
kind_string(int kind)
{
    switch (kind) {
    case NC_FORMAT_CLASSIC:
	return "classic";
    case NC_FORMAT_64BIT_OFFSET:
	return "64-bit offset";
    case NC_FORMAT_CDF5:
	return "cdf5";
    case NC_FORMAT_NETCDF4:
	return "netCDF-4";
    case NC_FORMAT_NETCDF4_CLASSIC:
	return "netCDF-4 classic model";
    default:
       error("unrecognized file format: %d", kind);
	return "unrecognized";
    }
}


/* Return extended format string */
static const char *
kind_string_extended(int kind, int mode)
{
    static char text[1024];
    switch (kind) {
    case NC_FORMATX_NC3:
	if(mode & NC_CDF5)
	    snprintf(text,sizeof(text),"%s mode=%08x", "64-bit data",mode);
	else if(mode & NC_64BIT_OFFSET)
	    snprintf(text,sizeof(text),"%s mode=%08x", "64-bit offset",mode);
	else
	    snprintf(text,sizeof(text),"%s mode=%08x", "classic",mode);
	break;
    case NC_FORMATX_NC_HDF5:
	snprintf(text,sizeof(text),"%s mode=%08x", "HDF5",mode);
	break;
    case NC_FORMATX_NC_HDF4:
	snprintf(text,sizeof(text),"%s mode=%08x", "HDF4",mode);
	break;
    case NC_FORMATX_PNETCDF:
	snprintf(text,sizeof(text),"%s mode=%08x", "PNETCDF",mode);
	break;
    case NC_FORMATX_DAP2:
	snprintf(text,sizeof(text),"%s mode=%08x", "DAP2",mode);
	break;
    case NC_FORMATX_DAP4:
	snprintf(text,sizeof(text),"%s mode=%08x", "DAP4",mode);
	break;
    case NC_FORMATX_UNDEFINED:
	snprintf(text,sizeof(text),"%s mode=%08x", "unknown",mode);
	break;
    default:
	error("unrecognized extended format: %d",kind);
	snprintf(text,sizeof(text),"%s mode=%08x", "unrecognized",mode);
	break;
    }
    return text;
}

#if 0
static int
fileopen(const char* path, void** memp, size_t* sizep)
{
    int status = NC_NOERR;
    int fd = -1;
    int oflags = 0;
    off_t size = 0;
    void* mem = NULL;
    off_t red = 0;
    char* pos = NULL;

    /* Open the file, but make sure we can write it if needed */
    oflags = O_RDONLY;
#ifdef O_BINARY
    oflags |= O_BINARY;
#endif
    oflags |= O_EXCL;
#ifdef vms
    fd = NCopen3(path, oflags, 0, "ctx=stm");
#else
    fd  = NCopen2(path, oflags);
#endif
    if(fd < 0) {
	status = errno;
	goto done;
    }
    /* get current filesize  = max(|file|,initialize)*/
    size = lseek(fd,0,SEEK_END);
    if(size < 0) {status = errno; goto done;}
    /* move pointer back to beginning of file */
    (void)lseek(fd,0,SEEK_SET);
    mem = malloc(size);
    if(mem == NULL) {status = NC_ENOMEM; goto done;}
    /* Read the file into memory */
    /* We need to do multiple reads because there is no
       guarantee that the amount read will be the full amount */
    red = size;
    pos = (char*)mem;
    while(red > 0) {
	ssize_t count = read(fd, pos, red);
	if(count < 0) {status = errno; goto done;}
        if(count == 0) {status = NC_ENOTNC; goto done;}
	/* assert(count > 0) */
	red -= count;
	pos += count;
    }

done:
    if(fd >= 0)
	(void)close(fd);
    if(status != NC_NOERR) {
#ifndef DEBUG
        fprintf(stderr,"open failed: file=%s err=%d\n",path,status);
	fflush(stderr);
#endif
    }
    if(status != NC_NOERR && mem != NULL) {
      free(mem);
      mem = NULL;
    } else {
      if(sizep) *sizep = size;
      if(memp) {
        *memp = mem;
      } else if(mem) {
        free(mem);
      }

    }


    return status;
}
#endif

/*
 * Emit initial line of output for NcML
 */
static void
pr_initx(int ncid, const char *path)
{
    printf("<?xml version=\"%s\" encoding=\"UTF-8\"?>\n<netcdf xmlns=\"https://www.unidata.ucar.edu/namespaces/netcdf/ncml-2.2\" location=\"%s\">\n",
	   XML_VERSION, path);
}

/*
 * Print attribute string, for text attributes.
 */
static void
pr_att_string(
    int kind,
    size_t len,
    const char *string
    )
{
    int iel;
    const char *cp;
    const char *sp;
    unsigned char uc;

    cp = string;
    printf ("\"");
    /* adjust len so trailing nulls don't get printed */
    sp = cp + len - 1;
    while (len != 0 && *sp-- == '\0')
	len--;
    for (iel = 0; iel < len; iel++)
	switch (uc = *cp++ & 0377) {
	case '\b':
	    printf ("\\b");
	    break;
	case '\f':
	    printf ("\\f");
	    break;
	case '\n':
	    /* Only generate linebreaks after embedded newlines for
	     * classic, 64-bit offset, cdf5, or classic model files.  For
	     * netCDF-4 files, don't generate linebreaks, because that
	     * would create an extra string in a list of strings.  */
	    if (kind != NC_FORMAT_NETCDF4) {
		printf ("\\n\",\n\t\t\t\"");
	    } else {
		printf("\\n");
	    }
	    break;
	case '\r':
	    printf ("\\r");
	    break;
	case '\t':
	    printf ("\\t");
	    break;
	case '\v':
	    printf ("\\v");
	    break;
	case '\\':
	    printf ("\\\\");
	    break;
	case '\'':
	    printf ("\\\'");
	    break;
	case '\"':
	    printf ("\\\"");
	    break;
	default:
	    if (iscntrl(uc))
	        printf ("\\%03o",uc);
	    else
	        printf ("%c",uc);
	    break;
	}
    printf ("\"");

}


/*
 * Print NcML attribute string, for text attributes.
 */
static void
pr_attx_string(
     const char* attname,
     size_t len,
     const char *string
     )
{
    int iel;
    const char *cp;
    const char *sp;
    unsigned char uc;
    int nulcount = 0;

    cp = string;
    printf ("\"");
    /* adjust len so trailing nulls don't get printed */
    sp = cp + len - 1;
    while (len != 0 && *sp-- == '\0')
	len--;
    for (iel = 0; iel < len; iel++)
	switch (uc = *cp++ & 0377) {
	case '\"':
	    printf ("&quot;");
	    break;
	case '<':
	    printf ("&lt;");
	    break;
	case '>':
	    printf ("&gt;");
	    break;
	case '&':
	    printf ("&amp;");
	    break;
	case '\n':
	    printf ("&#xA;");
	    break;
	case '\r':
	    printf ("&#xD;");
	    break;
	case '\t':
	    printf ("&#x9;");
	    break;
	case '\0':
	    printf ("&#0;");
	    if(nulcount++ == 0)
		fprintf(stderr,"Attribute: '%s'; value contains nul characters; producing illegal xml\n",attname);
	    break;
	default:
	    if (iscntrl(uc))
	        printf ("&#%d;",uc);
	    else
	        printf ("%c",uc);
	    break;
	}
    printf ("\"");

}


/*
 * Print list of attribute values, for attributes of primitive types.
 * Attribute values must be printed with explicit type tags for
 * netCDF-3 primitive types, because CDL doesn't require explicit
 * syntax to declare such attribute types.
 */
static void
pr_att_valgs(
    int kind,
    nc_type type,
    size_t len,
    const void *vals
    )
{
    int iel;
    signed char sc;
    short ss;
    int ii;
    char gps[PRIM_LEN];
    float ff;
    double dd;
    unsigned char uc;
    unsigned short us;
    unsigned int ui;
    int64_t i64;
    uint64_t ui64;
#ifdef USE_NETCDF4
    char *stringp;
#endif /* USE_NETCDF4 */
    char *delim = ", ";	/* delimiter between output values */

    if (type == NC_CHAR) {
	char *cp = (char *) vals;
	pr_att_string(kind, len, cp);
	return;
    }
    /* else */
    for (iel = 0; iel < len; iel++) {
	if (iel == len - 1)
	    delim = "";
	switch (type) {
	case NC_BYTE:
	    sc = ((signed char *) vals)[iel];
	    printf ("%db%s", sc, delim);
	    break;
	case NC_SHORT:
	    ss = ((short *) vals)[iel];
	    printf ("%ds%s", ss, delim);
	    break;
	case NC_INT:
	    ii = ((int *) vals)[iel];
	    printf ("%d%s", ii, delim);
	    break;
	case NC_FLOAT:
	    ff = ((float *) vals)[iel];
	    if(isfinite(ff)) {
		int res;
		res = snprintf(gps, PRIM_LEN, float_att_fmt, ff);
		assert(res < PRIM_LEN);
		tztrim(gps);	/* trim trailing 0's after '.' */
		printf ("%s%s", gps, delim);
	    } else {
		if(isnan(ff)) {
		    printf("NaNf%s", delim);
		} else if(isinf(ff)) {
		    if(ff < 0.0f) {
			printf("-");
		    }
		    printf("Infinityf%s", delim);
		}
	    }
	    break;
	case NC_DOUBLE:
	    dd = ((double *) vals)[iel];
	    if(isfinite(dd)) {
		int res;
		res = snprintf(gps, PRIM_LEN, double_att_fmt, dd);
		assert(res < PRIM_LEN);
		tztrim(gps);
		printf ("%s%s", gps, delim);
	    } else {
		if(isnan(dd)) {
		    printf("NaN%s", delim);
		} else if(isinf(dd)) {
		    if(dd < 0.0) {
			printf("-");
		    }
		    printf("Infinity%s", delim);
		}
	    }
	    break;
	case NC_UBYTE:
	    uc = ((unsigned char *) vals)[iel];
	    printf ("%uUB%s", uc, delim);
	    break;
	case NC_USHORT:
	    us = ((unsigned short *) vals)[iel];
	    printf ("%huUS%s", us, delim);
	    break;
	case NC_UINT:
	    ui = ((unsigned int *) vals)[iel];
	    printf ("%uU%s", ui, delim);
	    break;
	case NC_INT64:
	    i64 = ((int64_t *) vals)[iel];
	    printf ("%lldLL%s", i64, delim);
	    break;
	case NC_UINT64:
	    ui64 = ((uint64_t *) vals)[iel];
	    printf ("%lluULL%s", ui64, delim);
	    break;
#ifdef USE_NETCDF4
	case NC_STRING:
	    stringp = ((char **) vals)[iel];
            if(stringp)
                pr_att_string(kind, strlen(stringp), stringp);
            else
	        printf("NIL");
	    printf("%s", delim);
	    break;
#endif /* USE_NETCDF4 */
	default:
	    error("pr_att_vals: bad type");
	}
    }
}


/*
 * Print list of numeric attribute values to string for use in NcML output.
 * Unlike CDL, NcML makes type explicit, so don't need type suffixes.
 */
static void
pr_att_valsx(
     nc_type type,
     size_t len,
     const double *vals,
     char *attvals,		/* returned string */
     size_t attvalslen		/* size of attvals buffer, assumed
				   large enough to hold all len
				   blank-separated values */
     )
{
    int iel;
    float ff;
    double dd;
    int ii;
    unsigned int ui;
    int64_t i64;
    uint64_t ui64;

    attvals[0]='\0';
    if (len == 0)
	return;
    for (iel = 0; iel < len; iel++) {
	char gps[PRIM_LEN];
	int res;
	switch (type) {
	case NC_BYTE:
	case NC_SHORT:
	case NC_INT:
	    ii = vals[iel];
	    res = snprintf(gps, PRIM_LEN, "%d", ii);
	    assert(res < PRIM_LEN);
	    (void) strlcat(attvals, gps, attvalslen);
	    (void) strlcat(attvals, iel < len-1 ? " " : "", attvalslen);
	    break;
	case NC_UBYTE:
	case NC_USHORT:
	case NC_UINT:
	    ui = vals[iel];
	    res = snprintf(gps, PRIM_LEN, "%u", ui);
	    assert(res < PRIM_LEN);
	    (void) strlcat(attvals, gps, attvalslen);
	    (void) strlcat(attvals, iel < len-1 ? " " : "", attvalslen);
	    break;
	case NC_INT64:
	    i64 = vals[iel];
	    res = snprintf(gps, PRIM_LEN, "%lld", i64);
	    assert(res < PRIM_LEN);
	    (void) strlcat(attvals, gps, attvalslen);
	    (void) strlcat(attvals, iel < len-1 ? " " : "", attvalslen);
	    break;
	case NC_UINT64:
	    ui64 = vals[iel];
	    res = snprintf(gps, PRIM_LEN, "%llu", ui64);
	    assert(res < PRIM_LEN);
	    (void) strlcat(attvals, gps, attvalslen);
	    (void) strlcat(attvals, iel < len-1 ? " " : "", attvalslen);
	    break;
	case NC_FLOAT:
	    ff = vals[iel];
	    res = snprintf(gps, PRIM_LEN, float_attx_fmt, ff);
	    assert(res < PRIM_LEN);
	    tztrim(gps);	/* trim trailing 0's after '.' */
	    (void) strlcat(attvals, gps, attvalslen);
	    (void) strlcat(attvals, iel < len-1 ? " " : "", attvalslen);
	    break;
	case NC_DOUBLE:
	    dd = vals[iel];
	    res = snprintf(gps, PRIM_LEN, double_att_fmt, dd);
	    assert(res < PRIM_LEN);
	    tztrim(gps);	/* trim trailing 0's after '.' */
	    (void) strlcat(attvals, gps, attvalslen);
	    (void) strlcat(attvals, iel < len-1 ? " " : "", attvalslen);
	    break;
	default:
	    error("pr_att_valsx: bad type");
	}
    }
}

/*
 * Print a variable attribute
 */
static void
pr_att(
    int ncid,
    int kind,
    int varid,
    const char *varname,
    int ia
    )
{
    ncatt_t att;			/* attribute */

    NC_CHECK( nc_inq_attname(ncid, varid, ia, att.name) );
#ifdef USE_NETCDF4
    if (ncid == getrootid(ncid)
        && varid == NC_GLOBAL
        && strcmp(att.name,NCPROPS)==0)
	return; /* will be printed elsewhere */
#endif
    NC_CHECK( nc_inq_att(ncid, varid, att.name, &att.type, &att.len) );
    att.tinfo = get_typeinfo(att.type);

    indent_out();
    printf ("\t\t");
#ifdef USE_NETCDF4
    if (is_user_defined_type(att.type) || att.type == NC_STRING
        || (formatting_specs.xopt_filltype && varid != NC_GLOBAL && strcmp(_FillValue,att.name)==0))
#else
    if (is_user_defined_type(att.type))
#endif
    {
	/* TODO: omit next two lines if att_type_name not needed
	 * because print_type_name() looks it up */
	char att_type_name[NC_MAX_NAME + 1];
	get_type_name(ncid, att.type, att_type_name);

	/* printf ("\t\t%s ", att_type_name); */
	/* ... but handle special characters in CDL names with escapes */
	print_type_name(ncid, att.type);
	printf(" ");
    }
    /* 	printf ("\t\t%s:%s = ", varname, att.name); */
    print_name(varname);
    if(iskeyword(varname)) /* see discussion about escapes in ncgen man page*/
	printf(" ");
    printf(":");
    print_name(att.name);
    printf(" = ");

    if (att.len == 0) {	/* show 0-length attributes as empty strings */
	att.type = NC_CHAR;
    }

    if (! is_user_defined_type(att.type) ) {
	att.valgp = (void *) emalloc((att.len + 1) * att.tinfo->size );
	NC_CHECK( nc_get_att(ncid, varid, att.name, att.valgp ) );
	if(att.type == NC_CHAR)	/* null-terminate retrieved text att value */
	    ((char *)att.valgp)[att.len] = '\0';
/* (1) Print normal list of attribute values. */
        pr_att_valgs(kind, att.type, att.len, att.valgp);
	printf (" ;");			/* terminator for normal list */
/* (2) If -t option, add list of date/time strings as CDL comments. */
	if(formatting_specs.string_times) {
	    /* Prints text after semicolon and before final newline.
	     * Prints nothing if not qualified for time interpretation.
	     * Will include line breaks for longer lists. */
	    print_att_times(ncid, varid, &att);
	    if(is_bounds_att(&att)) {
		insert_bounds_info(ncid, varid, &att);
	    }
	}
#ifdef USE_NETCDF4
	/* If NC_STRING, need to free all the strings also */
	if(att.type == NC_STRING) {
	    nc_free_string(att.len, att.valgp);
	}
#endif /* USE_NETCDF4 */
	free(att.valgp);
    }
#ifdef USE_NETCDF4
    else /* User-defined type. */
    {
       char type_name[NC_MAX_NAME + 1];
       size_t type_size, nfields;
       nc_type base_nc_type;
       int class, i;
       void *data = NULL;

       NC_CHECK( nc_inq_user_type(ncid, att.type,  type_name, &type_size,
				  &base_nc_type, &nfields, &class));
       switch(class)
       {
	  case NC_VLEN:
	      /* because size returned for vlen is base type size, but we
	       * need space to read array of vlen structs into ... */
              data = emalloc((att.len + 1) * sizeof(nc_vlen_t));
	     break;
	  case NC_OPAQUE:
	      data = emalloc((att.len + 1) * type_size);
	     break;
	  case NC_ENUM:
	      /* a long long is ample for all base types */
	      data = emalloc((att.len + 1) * sizeof(int64_t));
	     break;
	  case NC_COMPOUND:
	      data = emalloc((att.len + 1) * type_size);
	     break;
	  default:
	     error("unrecognized class of user defined type: %d", class);
       }

       NC_CHECK( nc_get_att(ncid, varid, att.name, data));

       switch(class) {
       case NC_VLEN:
	   pr_any_att_vals(&att, data);
	   break;
       case NC_OPAQUE: {
	   char *sout = emalloc(2 * type_size + strlen("0X") + 1);
	   unsigned char *cp = data;
	   for (i = 0; i < att.len; i++) {
	       (void) ncopaque_val_as_hex(type_size, sout, cp);
	       printf("%s%s", sout, i < att.len-1 ? ", " : "");
	       cp += type_size;
	   }
	   free(sout);
         } break;
       case NC_ENUM: {
	   int64_t value;
	   for (i = 0; i < att.len; i++) {
	       char enum_name[NC_MAX_NAME + 1];
	       switch(base_nc_type)
	       {
	       case NC_BYTE:
		   value = *((char *)data + i);
		   break;
	       case NC_UBYTE:
		   value = *((unsigned char *)data + i);
		   break;
	       case NC_SHORT:
		   value = *((short *)data + i);
		   break;
	       case NC_USHORT:
		   value = *((unsigned short *)data + i);
		   break;
	       case NC_INT:
		   value = *((int *)data + i);
		   break;
	       case NC_UINT:
		   value = *((unsigned int *)data + i);
		   break;
	       case NC_INT64:
		   value = *((int64_t *)data + i);
		   break;
	       case NC_UINT64:
		   value = *((uint64_t *)data + i);
		   break;
	       default:
		   error("enum must have an integer base type: %d", base_nc_type);
	       }
	       NC_CHECK( nc_inq_enum_ident(ncid, att.type, value,
					   enum_name));
/* 	       printf("%s%s", enum_name, i < att.len-1 ? ", " : ""); */
	       print_name(enum_name);
	       printf("%s", i < att.len-1 ? ", " : "");
	   }
         } break;
       case NC_COMPOUND:
	   pr_any_att_vals(&att, data);
	   break;
       default:
	   error("unrecognized class of user defined type: %d", class);
       }
       NC_CHECK(nc_reclaim_data_all(ncid,att.type,data,att.len));
       printf (" ;");		/* terminator for user defined types */
    }
#endif /* USE_NETCDF4 */

    printf ("\n");		/* final newline for all attribute types */
}

/* Common code for printing attribute name */
static void
pr_att_name(
    int ncid,
    const char *varname,
    const char *attname
    )
{
    indent_out();
    printf ("\t\t");
    print_name(varname);
    printf(":");
    print_name(attname);
}

/*
 * Print special _Format global attribute, a virtual attribute not
 * actually stored in the file.
 */
static void
pr_att_global_format(
    int ncid,
    int kind
    )
{
    pr_att_name(ncid, "", NC_ATT_FORMAT);
    printf(" = ");
    printf("\"%s\"", kind_string(kind));
    printf (" ;\n");
}

#ifdef USE_NETCDF4
/*
 * Print special reserved variable attributes, such as _Chunking,
 * _DeflateLevel, ...  These are virtual, not real, attributes
 * generated from the result of inquire calls.  They are of primitive
 * type to fit into the classic model.  Currently, these only exist
 * for netCDF-4 data.
 */
static void
pr_att_specials(
    int ncid,
    int kind,
    int varid,
    const ncvar_t *varp
    )
{
    int contig = NC_CHUNKED;
    /* No special variable attributes for classic or 64-bit offset data */
    if(kind == 1 || kind == 2)
	return;
    /* _Chunking tests */
    NC_CHECK( nc_inq_var_chunking(ncid, varid, &contig, NULL ) );
    if(contig == NC_CONTIGUOUS) {
  	    pr_att_name(ncid, varp->name, NC_ATT_STORAGE);
	    printf(" = \"contiguous\" ;\n");
    } else if(contig == NC_COMPACT) {
	    pr_att_name(ncid, varp->name, NC_ATT_STORAGE);
	    printf(" = \"compact\" ;\n");
    } else if(contig == NC_CHUNKED) {
 	   size_t *chunkp;
	   int i;
	    pr_att_name(ncid, varp->name, NC_ATT_STORAGE);
	    printf(" = \"chunked\" ;\n");
	    chunkp = (size_t *) emalloc(sizeof(size_t) * (varp->ndims + 1) );
	    NC_CHECK( nc_inq_var_chunking(ncid, varid, NULL, chunkp) );
	    /* print chunking, even if it is default */
	    pr_att_name(ncid, varp->name, NC_ATT_CHUNKING);
	    printf(" = ");
	    for(i = 0; i < varp->ndims; i++) {
		printf("%lu%s", (unsigned long)chunkp[i], i+1 < varp->ndims ? ", " : " ;\n");
	    }
	    free(chunkp);
    } else if(contig == NC_VIRTUAL) {
	    pr_att_name(ncid, varp->name, NC_ATT_STORAGE);
	    printf(" = \"virtual\" ;\n");
    } else {
	    pr_att_name(ncid, varp->name, NC_ATT_STORAGE);
	    printf(" = \"unknown\" ;\n");
    }

    /* _Checksum (fletcher32) */
    if(!formatting_specs.filter_atts) {
	int fletcher32 = 0;
	NC_CHECK( nc_inq_var_fletcher32(ncid, varid, &fletcher32) );
	if(fletcher32 != 0) {
	    pr_att_name(ncid, varp->name, NC_ATT_CHECKSUM);
	    printf(" = \"true\" ;\n");
	}
    }
    /* _Shuffle */
    if(!formatting_specs.filter_atts) {
	int haveshuffle = 0;
	NC_CHECK( nc_inq_var_deflate(ncid, varid, &haveshuffle, NULL, NULL));
	if(haveshuffle) {
	    pr_att_name(ncid, varp->name, NC_ATT_SHUFFLE);
	    printf(" = \"true\" ;\n");
	}
    }

    /* _Deflate*/
    if(!formatting_specs.filter_atts) {
	int havedeflate = 0;
	int level = -1;
	NC_CHECK( nc_inq_var_deflate(ncid, varid, NULL, &havedeflate, &level));
	if(havedeflate) {
	    pr_att_name(ncid, varp->name, NC_ATT_DEFLATE);
	    printf(" = %d ;\n",level);
	}
    }

    /* _Filter */
    {
	size_t nparams, nfilters, nbytes;
	unsigned int* filterids = NULL;
	unsigned int* params = NULL;

	/* Get applicable filter ids */
	NC_CHECK(nc_inq_var_filter_ids(ncid, varid, &nfilters, NULL));
	/* Get set of filters for this variable */
        filterids = NULL;
	if(nfilters > 0) {
	    filterids = (unsigned int*)malloc(sizeof(unsigned int)*nfilters);
	    if(filterids == NULL) NC_CHECK(NC_ENOMEM);
	    NC_CHECK(nc_inq_var_filter_ids(ncid, varid, &nfilters, filterids));
	}
        if(nfilters > 0) {
	    int k;
	    int first = 1;
	    int _filter = 0;
	    for(k=0;k<nfilters;k++) {
		NC_CHECK(nc_inq_var_filter_info(ncid, varid, filterids[k], &nparams, NULL));
		if(!formatting_specs.filter_atts && (
	  	      filterids[k] == H5Z_FILTER_FLETCHER32
		   || filterids[k] == H5Z_FILTER_SHUFFLE
   		   || filterids[k] == H5Z_FILTER_DEFLATE))
		    continue; /* Ignore fletcher32 and shuffle and deflate*/
		_filter = 1;
	        if(nparams > 0) {
  	            params = (unsigned int*)calloc(1,sizeof(unsigned int)*nparams);
	            NC_CHECK(nc_inq_var_filter_info(ncid, varid, filterids[k], &nbytes, params));
		} else
		    params = NULL;
		if(first) {
		    pr_att_name(ncid,varp->name,NC_ATT_FILTER);
		    printf(" = \"");
		} else 
	            printf("|");
		printf("%u",filterids[k]);
		if(nparams > 0) {
	            int i;
		    for(i=0;i<nparams;i++)
		        printf(",%u",params[i]);
	        }
       	        nullfree(params); params = NULL;
		first = 0;
	    }
            if(_filter) printf("\" ;\n");
	}
	if(filterids) free(filterids);
    }
    /* _Codecs*/
    {
	int stat;
	size_t len;
	nc_type typeid;
        stat = nc_inq_att(ncid,varid,NC_ATT_CODECS,&typeid,&len);
        if(stat == NC_NOERR && typeid == NC_CHAR && len > 0) {
	    char* json = (char*)malloc(len+1);
	    if(json != NULL) {	    
                stat = nc_get_att_text(ncid,varid,NC_ATT_CODECS,json);
                if(stat == NC_NOERR) {
		    char* escapedjson = NULL;
		    pr_att_name(ncid, varp->name, NC_ATT_CODECS);
		    /* Escape the json */
		    escapedjson = escaped_string(json);	
                    printf(" = \"%s\" ;\n",escapedjson);
		    free(escapedjson);
		}
		free(json);
	    }
	}
    }
    /* _Endianness */
    if(varp->tinfo->size > 1) /* Endianness is meaningless for 1-byte types */
    {
	int endianness = 0;
	NC_CHECK( nc_inq_var_endian(ncid, varid, &endianness) );
	if (endianness != NC_ENDIAN_NATIVE) { /* NC_ENDIAN_NATIVE is the default */
	    pr_att_name(ncid, varp->name, NC_ATT_ENDIANNESS);
	    printf(" = ");
	    switch (endianness) {
	    case NC_ENDIAN_LITTLE:
		printf("\"little\"");
		break;
	    case NC_ENDIAN_BIG:
		printf("\"big\"");
		break;
	    default:
		error("pr_att_specials: bad endianness: %d", endianness);
		break;
	    }
	    printf(" ;\n");
	}
    }
    {
	int no_fill = 0;
	/* Don't get the fill_value, it's set explicitly with
	 * _FillValue attribute, because nc_def_var_fill() creates a
	 * _FillValue attribute, if needed, and it's value gets
	 * displayed elsewhere as a normal (not special virtual)
	 * attribute. */
	NC_CHECK( nc_inq_var_fill(ncid, varid, &no_fill, NULL) );
	if(no_fill != 0) {
	    pr_att_name(ncid, varp->name, NC_ATT_NOFILL);
	    printf(" = \"true\" ;\n");
	}
    }

    /* TODO: handle _Nbit when inquire function is available */

    /* TODO: handle _ScaleOffset when inquire is available */

    /* TODO: handle _Szip when szip inquire function is available */
}
#endif /* USE_NETCDF4 */


static void
pr_att_hidden(
    int ncid,
    int kind
    )
{
    int stat;
    size_t len;

    /* No special variable attributes for classic or 64-bit offset data */
#if 0
    if(kind == 1 || kind == 2) return;
#endif
    /* Print out Selected hidden attributes */
    /* NCPROPS */
    stat = nc_inq_att(ncid,NC_GLOBAL,NCPROPS,NULL,&len);
    if(stat == NC_NOERR) {
	char* propdata = (char*)malloc(len+1);
	if(propdata == NULL)
	    return;
        stat = nc_get_att_text(ncid,NC_GLOBAL,NCPROPS,propdata);
        if(stat == NC_NOERR) {
            pr_att_name(ncid, "", NCPROPS);
            /* make sure its null terminated */
            propdata[len] = '\0';
            printf(" = \"%s\" ;\n",propdata);
        }
	free(propdata);
    }
    /* _SuperblockVersion */
    stat = nc_inq_att(ncid,NC_GLOBAL,SUPERBLOCKATT,NULL,&len);
    if(stat == NC_NOERR && len == 1) {
        int sbversion;
        stat = nc_get_att_int(ncid,NC_GLOBAL,SUPERBLOCKATT,&sbversion);
        if(stat == NC_NOERR) {
            pr_att_name(ncid, "", SUPERBLOCKATT);
            printf(" = %d ;\n",sbversion);
        }
    }
    /* _IsNetcdf4 */
    stat = nc_inq_att(ncid,NC_GLOBAL,ISNETCDF4ATT,NULL,&len);
    if(stat == NC_NOERR && len == 1) {
        int isnc4;
        stat = nc_get_att_int(ncid,NC_GLOBAL,ISNETCDF4ATT,&isnc4);
        if(stat == NC_NOERR) {
            pr_att_name(ncid, "", ISNETCDF4ATT);
            printf(" = %d ;\n",isnc4?1:0);
        }
    }
}

/*
 * Print a variable attribute for NcML
 */
static void
pr_attx(
    int ncid,
    int varid,
    int ia
    )
{
    ncatt_t att;			/* attribute */
    char *attvals = NULL;
    int attvalslen = 0;

    NC_CHECK( nc_inq_attname(ncid, varid, ia, att.name) );
#ifdef USE_NETCDF4
    if (ncid == getrootid(ncid)
	&& varid == NC_GLOBAL
        && strcmp(att.name,NCPROPS)==0
        && (!formatting_specs.special_atts
            || !formatting_specs.xopt_props)
	)
	return;
#endif
    NC_CHECK( nc_inq_att(ncid, varid, att.name, &att.type, &att.len) );

    /* Put attribute values into a single string, with blanks in between */

    switch (att.type) {
    case NC_CHAR:
	attvals = (char *) emalloc(att.len + 1);
	attvalslen = att.len;
	attvals[att.len] = '\0';
	NC_CHECK( nc_get_att_text(ncid, varid, att.name, attvals ) );
	break;
#ifdef USE_NETCDF4
    case NC_STRING:
	/* TODO: this only prints first string value, need to handle
	   multiple strings? */
	attvals = (char *) emalloc(att.len + 1);
	attvals[att.len] = '\0';
	NC_CHECK( nc_get_att_text(ncid, varid, att.name, attvals ) );
	break;
    case NC_VLEN:
	/* TODO */
	break;
    case NC_OPAQUE:
	/* TODO */
	break;
    case NC_COMPOUND:
	/* TODO */
	break;
#endif /* USE_NETCDF4 */
    default:
	att.vals = (double *) emalloc((att.len + 1) * sizeof(double));
	NC_CHECK( nc_get_att_double(ncid, varid, att.name, att.vals ) );
	attvalslen = PRIM_LEN * att.len; /* max chars for each value and blank separator */
	attvals = (char *) emalloc(attvalslen + 1);
	pr_att_valsx(att.type, att.len, att.vals, attvals, attvalslen);
	free(att.vals);
	break;
    }

    /* Don't output type for string attributes, since that's default type */
    if(att.type == NC_CHAR
#ifdef USE_NETCDF4
                          || att.type == NC_CHAR
#endif /* USE_NETCDF4 */
       ) {
	/* TODO: XML-ish escapes for special chars in names */
	printf ("%s  <attribute name=\"%s\" value=",
		varid != NC_GLOBAL ? "  " : "",
		att.name);
	/* print attvals as a string with XML escapes */
	pr_attx_string(att.name, attvalslen, attvals);
    } else {			/* non-string attribute */
	char att_type_name[NC_MAX_NAME + 1];
	get_type_name(ncid, att.type, att_type_name);
	/* TODO: print full type name with group prefix, when needed */
	printf ("%s  <attribute name=\"%s\" type=\"%s\" value=\"",
		varid != NC_GLOBAL ? "  " : "",
		att.name,
		att_type_name);
	printf("%s\"",attvals);
    }
    printf (" />\n");
    if(attvals != NULL)
      free (attvals);
}


/* Print optional NcML attribute for a variable's shape */
static void
pr_shape(ncvar_t* varp, ncdim_t *dims)
{
    char *shape;
    int shapelen = 0;
    int id;

    if (varp->ndims == 0)
	return;
    for (id = 0; id < varp->ndims; id++) {
	shapelen += strlen(dims[varp->dims[id]].name) + 1;
    }
    shape = (char *) emalloc(shapelen + 1);
    shape[0] = '\0';
    for (id = 0; id < varp->ndims; id++) {
	/* TODO: XML-ish escapes for special chars in dim names */
	strlcat(shape, dims[varp->dims[id]].name, shapelen);
	strlcat(shape, id < varp->ndims-1 ? " " : "", shapelen);
    }
    printf (" shape=\"%s\"", shape);
    free(shape);
}

#ifdef USE_NETCDF4


/* Print an enum type declaration */
static void
print_enum_type(int ncid, nc_type typeid) {
    char type_name[NC_MAX_NAME + 1];
    size_t type_size;
    nc_type base_nc_type;
    size_t type_nfields;
    int type_class;
    char base_type_name[NC_MAX_NAME + 1];
    int f;
    int64_t memval;
    char memname[NC_MAX_NAME + 1];
 /* extra space for escapes, and punctuation */
#define SAFE_BUF_LEN 4*NC_MAX_NAME+30
    char safe_buf[SAFE_BUF_LEN];
    char *delim;
    int64_t data;	    /* space for data of any primitive type */
    void* raw;
    char *esc_btn;
    char *esc_tn;
    char *esc_mn;
    int res;

    NC_CHECK( nc_inq_user_type(ncid, typeid, type_name, &type_size, &base_nc_type,
			       &type_nfields, &type_class) );

    get_type_name(ncid, base_nc_type, base_type_name);
    indent_out();
    esc_btn = escaped_name(base_type_name);
    esc_tn = escaped_name(type_name);
    res = snprintf(safe_buf, SAFE_BUF_LEN,"%s enum %s {", esc_btn, esc_tn);
    assert(res < SAFE_BUF_LEN);
    free(esc_btn);
    free(esc_tn);
    lput(safe_buf);
    delim = ", ";
    for (f = 0; f < type_nfields; f++) {
	if (f == type_nfields - 1)
	    delim = "} ;\n";
	NC_CHECK( nc_inq_enum_member(ncid, typeid, f, memname, &data) );
	raw = (void*)&data;
	switch (base_nc_type) {
	case NC_BYTE:
	    memval = *(char *)raw;
	    break;
	case NC_SHORT:
	    memval = *(short *)raw;
	    break;
	case NC_INT:
	    memval = *(int *)raw;
	    break;
	case NC_UBYTE:
	    memval = *(unsigned char *)raw;
	    break;
	case NC_USHORT:
	    memval = *(unsigned short *)raw;
	    break;
	case NC_UINT:
	    memval = *(unsigned int *)raw;
	    break;
	case NC_INT64:
	    memval = *(int64_t *)raw;
	    break;
	case NC_UINT64:
	    memval = *(uint64_t *)raw;
	    break;
	default:
	    error("Bad base type for enum!");
	    break;
	}
	esc_mn = escaped_name(memname);
	res = snprintf(safe_buf, SAFE_BUF_LEN, "%s = %lld%s", esc_mn,
		       memval, delim);
	assert(res < SAFE_BUF_LEN);
	free(esc_mn);
	lput(safe_buf);
    }
}


/* Print a user-defined type declaration */
static void
print_ud_type(int ncid, nc_type typeid) {

    char type_name[NC_MAX_NAME + 1];
    char base_type_name[NC_MAX_NAME + 1];
    size_t type_nfields, type_size;
    nc_type base_nc_type;
    int f, type_class;

    NC_CHECK( nc_inq_user_type(ncid, typeid, type_name, &type_size, &base_nc_type,
			       &type_nfields, &type_class) );
    switch(type_class) {
    case NC_VLEN:
	/* TODO: don't bother getting base_type_name if
	 * print_type_name looks it up anyway */
	get_type_name(ncid, base_nc_type, base_type_name);
	indent_out();
/* 	printf("%s(*) %s ;\n", base_type_name, type_name); */
	print_type_name(ncid, base_nc_type);
	printf("(*) ");
	print_type_name(ncid, typeid);
	printf(" ;\n");
	break;
    case NC_OPAQUE:
	indent_out();
/* 	printf("opaque(%d) %s ;\n", (int)type_size, type_name); */
	printf("opaque(%d) ", (int)type_size);
	print_type_name(ncid, typeid);
	printf(" ;\n");
	break;
    case NC_ENUM:
	print_enum_type(ncid, typeid);
	break;
    case NC_COMPOUND:
	{
	    char field_name[NC_MAX_NAME + 1];
	    char field_type_name[NC_MAX_NAME + 1];
	    size_t field_offset;
	    nc_type field_type;
	    int field_ndims;
	    int d;

	    indent_out();
/* 	    printf("compound %s {\n", type_name); */
	    printf("compound ");
	    print_type_name(ncid, typeid);
	    printf(" {\n");
	    for (f = 0; f < type_nfields; f++)
		{
		    NC_CHECK( nc_inq_compound_field(ncid, typeid, f, field_name,
						    &field_offset, &field_type,
						    &field_ndims, NULL) );
		    /* TODO: don't bother if field_type_name not needed here */
		    get_type_name(ncid, field_type, field_type_name);
		    indent_out();
/* 		    printf("  %s %s", field_type_name, field_name); */
		    printf("  ");
		    print_type_name(ncid, field_type);
		    printf(" ");
		    print_name(field_name);
		    if (field_ndims > 0) {
			int *field_dim_sizes = (int *) emalloc((field_ndims + 1) * sizeof(int));
			NC_CHECK( nc_inq_compound_field(ncid, typeid, f, NULL,
							NULL, NULL, NULL,
							field_dim_sizes) );
			printf("(");
			for (d = 0; d < field_ndims-1; d++)
			    printf("%d, ", field_dim_sizes[d]);
			printf("%d)", field_dim_sizes[field_ndims-1]);
			free(field_dim_sizes);
		    }
		    printf(" ;\n");
		}
            indent_out();
#if 0
 	    printf("}; // %s\n", type_name);
#else
	    printf("}; // ");
#endif
	    print_type_name(ncid, typeid);
	    printf("\n");
	}
	break;
    default:
	error("Unknown class of user-defined type!");
    }
}
#endif /* USE_NETCDF4 */

static void
get_fill_info(int ncid, int varid, ncvar_t *vp)
{
    ncatt_t att;			/* attribute */
    int nc_status;			/* return from netcdf calls */
    void *fillvalp = NULL;

    vp->has_fillval = 1; /* by default, but turn off for bytes */

    /* get _FillValue attribute */
    nc_status = nc_inq_att(ncid,varid,_FillValue,&att.type,&att.len);
    fillvalp = ecalloc(vp->tinfo->size + 1);
    if(nc_status == NC_NOERR &&
       att.type == vp->type && att.len == 1) {
	NC_CHECK(nc_get_att(ncid, varid, _FillValue, fillvalp));
    } else {
	switch (vp->type) {
	case NC_BYTE:
	    /* don't do default fill-values for bytes, too risky */
	    vp->has_fillval = 0;
	    free(fillvalp);
	    fillvalp = 0;
	    break;
	case NC_CHAR:
	    *(char *)fillvalp = NC_FILL_CHAR;
	    break;
	case NC_SHORT:
	    *(short *)fillvalp = NC_FILL_SHORT;
	    break;
	case NC_INT:
	    *(int *)fillvalp = NC_FILL_INT;
	    break;
	case NC_FLOAT:
	    *(float *)fillvalp = NC_FILL_FLOAT;
	    break;
	case NC_DOUBLE:
	    *(double *)fillvalp = NC_FILL_DOUBLE;
	    break;
	case NC_UBYTE:
	    /* don't do default fill-values for bytes, too risky */
	    vp->has_fillval = 0;
	    free(fillvalp);
	    fillvalp = 0;
	    break;
	case NC_USHORT:
	    *(unsigned short *)fillvalp = NC_FILL_USHORT;
	    break;
	case NC_UINT:
	    *(unsigned int *)fillvalp = NC_FILL_UINT;
	    break;
	case NC_INT64:
	    *(int64_t *)fillvalp = NC_FILL_INT64;
	    break;
	case NC_UINT64:
	    *(uint64_t *)fillvalp = NC_FILL_UINT64;
	    break;
#ifdef USE_NETCDF4
	case NC_STRING: {
	    char* s;
	    size_t len = strlen(NC_FILL_STRING);
#if 0
	    /* In order to avoid mem leak, allocate this string as part of fillvalp */
            fillvalp = erealloc(fillvalp, vp->tinfo->size + 1 + len + 1);
	    s = ((char*)fillvalp) + vp->tinfo->size + 1;
#else
	    s = malloc(len+1);
#endif
	    memcpy(s,NC_FILL_STRING,len);
	    s[len] = '\0';
	    *((char **)fillvalp) = s;
	    } break;
#endif /* USE_NETCDF4 */
	default:		/* no default fill values for NC_NAT
				   or user-defined types */
	    vp->has_fillval = 0;
	    free(fillvalp);
	    fillvalp = 0;
	    break;
	}
    }
    vp->fillvalp = fillvalp;
}

/* Recursively dump the contents of a group. (Only netcdf-4 format
 * files can have groups, so recursion will not take place for classic
 * format files.)
 *
 * ncid: id of open file (first call) or group (subsequent recursive calls)
 * path: file path name (first call)
 */
static void
do_ncdump_rec(int ncid, const char *path)
{
   int ndims;			/* number of dimensions */
   int nvars;			/* number of variables */
   int ngatts;			/* number of global attributes */
   int xdimid;			/* id of unlimited dimension */
   int varid;			/* variable id */
   int rootncid;		/* id of root group */
   ncdim_t *dims;		/* dimensions */
   size_t *vdims=0;	        /* dimension sizes for a single variable */
   ncvar_t var;			/* variable */
   int id;			/* dimension number per variable */
   int ia;			/* attribute number */
   int iv;			/* variable number */
   idnode_t* vlist = NULL;	/* list for vars specified with -v option */
   char type_name[NC_MAX_NAME + 1];
   int kind;		/* strings output differently for nc4 files */
   char dim_name[NC_MAX_NAME + 1];
#ifdef USE_NETCDF4
   int *dimids_grp;	        /* dimids of the dims in this group. */
   int *unlimids;		/* dimids of unlimited dimensions in this group */
   int d_grp, ndims_grp;
   int ntypes, *typeids;
   int nunlim;
#else
   int dimid;			/* dimension id */
#endif /* USE_NETCDF4 */
   int is_root = 1;		/* true if ncid is root group or if netCDF-3 */

#ifdef USE_NETCDF4
   if (nc_inq_grp_parent(ncid, NULL) != NC_ENOGRP)
       is_root = 0;
#endif /* USE_NETCDF4 */
   NC_CHECK(nc_inq_ncid(ncid,NULL,&rootncid)); /* get root group ncid */

   /*
    * If any vars were specified with -v option, get list of
    * associated variable ids relative to this group.  Assume vars
    * specified with syntax like "grp1/grp2/varname" or
    * "/grp1/grp2/varname" if they are in groups.
    */
   if (formatting_specs.nlvars > 0) {
      vlist = newidlist();	/* list for vars specified with -v option */
      for (iv=0; iv < formatting_specs.nlvars; iv++) {
	  if(nc_inq_gvarid(ncid, formatting_specs.lvars[iv], &varid) == NC_NOERR)
	      idadd(vlist, varid);
      }
   }

#ifdef USE_NETCDF4
   /* Are there any user defined types in this group? */
   NC_CHECK( nc_inq_typeids(ncid, &ntypes, NULL) );
   if (ntypes)
   {
      int t;

      typeids = emalloc((ntypes + 1) * sizeof(int));
      NC_CHECK( nc_inq_typeids(ncid, &ntypes, typeids) );
      indent_out();
      printf("types:\n");
      indent_more();
      for (t = 0; t < ntypes; t++)
      {
	 print_ud_type(ncid, typeids[t]); /* print declaration of user-defined type */
      }
      indent_less();
      free(typeids);
   }
#endif /* USE_NETCDF4 */

   /*
    * get number of dimensions, number of variables, number of global
    * atts, and dimension id of unlimited dimension, if any
    */
   NC_CHECK( nc_inq(ncid, &ndims, &nvars, &ngatts, &xdimid) );
   /* get dimension info */
   dims = (ncdim_t *) emalloc((ndims + 1) * sizeof(ncdim_t));
   if (ndims > 0) {
       indent_out();
       printf ("dimensions:\n");
   }

#ifdef USE_NETCDF4
   /* In netCDF-4 files, dimids will not be sequential because they
    * may be defined in various groups, and we are only looking at one
    * group at a time. */

   /* Find the number of dimids defined in this group. */
   NC_CHECK( nc_inq_ndims(ncid, &ndims_grp) );
   dimids_grp = (int *)emalloc((ndims_grp + 1) * sizeof(int));

   /* Find the dimension ids in this group. */
   NC_CHECK( nc_inq_dimids(ncid, 0, dimids_grp, 0) );

   /* Find the number of unlimited dimensions and get their IDs */
   NC_CHECK( nc_inq_unlimdims(ncid, &nunlim, NULL) );
   unlimids = (int *)emalloc((nunlim + 1) * sizeof(int));
   NC_CHECK( nc_inq_unlimdims(ncid, &nunlim, unlimids) );

   /* For each dimension defined in this group, get and print out info. */
   for (d_grp = 0; d_grp < ndims_grp; d_grp++)
   {
      int dimid = dimids_grp[d_grp];
      int is_unlimited = 0;
      int uld;
      int stat;

      for (uld = 0; uld < nunlim; uld++) {
	  if(dimid == unlimids[uld]) {
	      is_unlimited = 1;
	      break;
	  }
      }
      stat = nc_inq_dim(ncid, dimid, dims[d_grp].name, &dims[d_grp].size);
      if (stat == NC_EDIMSIZE && SIZEOF_SIZE_T < 8) {
	  error("dimension \"%s\" too large for 32-bit platform, try 64-bit version", dims[d_grp].name);
      } else {
	  NC_CHECK (stat);
      }
      indent_out();
      printf ("\t");
      print_name(dims[d_grp].name);
      printf (" = ");
      if(SIZEOF_SIZE_T >= 8) {
	  if (is_unlimited) {
	      printf ("UNLIMITED ; // (%lu currently)\n",
		      (unsigned long)dims[d_grp].size);
	  } else {
	      printf ("%lu ;\n", (unsigned long)dims[d_grp].size);
	  }
      } else {			/* 32-bit platform */
	  if (is_unlimited) {
	      printf ("UNLIMITED ; // (%u currently)\n",
		      (unsigned int)dims[d_grp].size);
	  } else {
	      printf ("%u ;\n", (unsigned int)dims[d_grp].size);
	  }
      }
   }
   if(unlimids)
       free(unlimids);
   if(dimids_grp)
       free(dimids_grp);
#else /* not using netCDF-4 */
   for (dimid = 0; dimid < ndims; dimid++) {
      NC_CHECK( nc_inq_dim(ncid, dimid, dims[dimid].name, &dims[dimid].size) );
      indent_out();
      printf ("\t");
      print_name(dims[dimid].name);
      printf (" = ");
      if (dimid == xdimid) {
	  printf ("UNLIMITED ; // (%u currently)\n",
		  (unsigned int)dims[dimid].size);
      } else {
	  printf ("%llu ;\n", (unsigned long long)dims[dimid].size);
      }
   }
#endif /* USE_NETCDF4 */

   if (nvars > 0) {
       indent_out();
       printf ("variables:\n");
   }
   /* Because netCDF-4 can have a string attribute with multiple
    * string values, we can't output strings with embedded newlines
    * as what look like multiple strings, as we do for classic and
    * 64-bit offset  and cdf5 files.  So we need to know the output file type
    * to know how to print strings with embedded newlines. */
   NC_CHECK( nc_inq_format(ncid, &kind) );

   memset((void*)&var,0,sizeof(var));

   /* For each var, get and print out info. */

   for (varid = 0; varid < nvars; varid++) {
      NC_CHECK( nc_inq_varndims(ncid, varid, &var.ndims) );
      var.dims = (int *) emalloc((var.ndims + 1) * sizeof(int));
      NC_CHECK( nc_inq_var(ncid, varid, var.name, &var.type, 0,
			   var.dims, &var.natts) );
      /* TODO: don't bother if type name not needed here */
      get_type_name(ncid, var.type, type_name);
      var.tinfo = get_typeinfo(var.type);
      indent_out();
/*       printf ("\t%s %s", type_name, var.name); */
      printf ("\t");
      /* TODO: if duplicate type name and not just inherited, print
       * full type name. */
      print_type_name (ncid, var.type);
      printf (" ");
      print_name (var.name);
      if (var.ndims > 0)
	 printf ("(");
      for (id = 0; id < var.ndims; id++) {
	 /* Get the base name of the dimension */
	 NC_CHECK( nc_inq_dimname(ncid, var.dims[id], dim_name) );
#ifdef USE_NETCDF4
	 /* This dim may be in a parent group, so let's look up dimid
	  * parent group; if it is not current group, then we will print
	  * the fully qualified name.
	  * Subtlety: The following code block is needed because
	  * nc_inq_dimname() currently returns only a simple dimension
	  * name, without a prefix identifying the group it came from.
	  * That's OK unless the dimid identifies a dimension in an
	  * ancestor group that has the same simple name as a
	  * dimension in the current group (or some intermediate
	  * group), in which case the simple name is ambiguous.  This
	  * code tests for that case and provides an absolute dimname
	  * only in the case where a simple name would be
	  * ambiguous.
	  * The algorithm is as follows:
	  * 1. Search up the tree of ancestor groups.
	  * 2. If one of those groups contains the dimid, then call it dimgrp.
	  * 3. If one of those groups contains a dim with the same name as the dimid,
	  *    but with a different dimid, then record that as duplicate=true.
	  * 4. If dimgrp is defined and duplicate == false, then we do not need an fqn.
	  * 5. If dimgrp is defined and duplicate == true, then we do need an fqn to avoid using the duplicate.
	  * 6. if dimgrp is undefined, then do a preorder breadth-first search of all the groups looking for the
	  *    dimid.
	  * 7. If found, then use the fqn of that dimension location.
  	  * 8. If not found, then signal NC_EBADDIM.
          */	 

	  int target_dimid, dimgrp, duplicate, stopsearch, usefqn;

	  target_dimid = var.dims[id];
	  dimgrp = ncid; /* start with the parent group of the variable */
	  duplicate = 0;
	  usefqn = 0;

	  /* Walk up the ancestor groups */
	  for(stopsearch=0;stopsearch==0;) {
	     int tmpid;
	     int localdimid;
	     int ret = NC_NOERR;
	     ret = nc_inq_dimid(dimgrp,dim_name,&localdimid);
	     switch (ret) {
	     case NC_NOERR: /* We have a name match */
	         if(localdimid == target_dimid) stopsearch = 1; /* 1 means stop because found */
		 else duplicate = 1;
		 break;
	     case NC_EBADDIM:
		 break; /* no match at all */
	     default: NC_CHECK(ret);
	     }
	     if(stopsearch != 0) break; /* no need to continue */
	     /* move to ancestor group */
	     ret = nc_inq_grp_parent(dimgrp,&tmpid);
	     switch(ret) {
	     case NC_NOERR:
	         dimgrp = tmpid;
		 break;
	     case NC_ENOGRP:
		 /* we processed the root, so try the breadth-first search */
		 stopsearch = -1; /* -1 means we hit the root group but did not find it */
		 rootncid = dimgrp;
		 break;		 
	     default: NC_CHECK(ret);
	     }
	  }
	  assert(stopsearch != 0);
	  if(stopsearch == 1) {
	      /* We found it; do we need to use fqn */
	      usefqn = duplicate;
	  } else { /* stopsearch == -1 */
	      /* do the whole-tree search */
	      usefqn = 1;
	      NC_CHECK(searchgrouptreedim(rootncid,target_dimid,&dimgrp));
              /* group containing target dimid is in group dimgrp */
	  }
	  if(usefqn) {
             /* use fully qualified name (fqn) for the dimension name by prefixing dimname
                with group name */
	      size_t len;
	      char *grpfqn = NULL;	/* the group fqn */
	      NC_CHECK( nc_inq_grpname_full(dimgrp, &len, NULL) );
	      grpfqn = emalloc(len + 1);
	      NC_CHECK( nc_inq_grpname_full(dimgrp, &len, grpfqn) );
	      print_name (grpfqn);
	      if(strcmp("/", grpfqn) != 0) /* not the root group */
	          printf("/");		 /* ensure a trailing slash */
	      free(grpfqn);
	   }
#endif /*USE_NETCDF4*/
	   print_name (dim_name);
	   printf ("%s", id < var.ndims-1 ? ", " : RPAREN);
      }
      printf (" ;\n");

      /* print variable attributes */
      for (ia = 0; ia < var.natts; ia++) { /* print ia-th attribute */
	  pr_att(ncid, kind, varid, var.name, ia);
      }
#ifdef USE_NETCDF4
      /* Print special (virtual) attributes, if option specified */
      if (formatting_specs.special_atts) {
	  pr_att_specials(ncid, kind, varid, &var);
      }
#endif /* USE_NETCDF4 */
      if(var.dims) {free((void*)var.dims); var.dims = NULL;}
   }

   if (ngatts > 0 || formatting_specs.special_atts) {
      printf ("\n");
      indent_out();
      if (is_root)
	  printf("// global attributes:\n");
      else
	  printf("// group attributes:\n");
   }
   for (ia = 0; ia < ngatts; ia++) { /* print ia-th global attribute */
       pr_att(ncid, kind, NC_GLOBAL, "", ia);
   }
   if (is_root && formatting_specs.special_atts) { /* output special attribute
					   * for format variant */

     pr_att_hidden(ncid, kind);
       pr_att_global_format(ncid, kind);
   }

   fflush(stdout);

   /* output variable data, unless "-h" option specified header only
    * or this group is not in list of groups specified by "-g"
    * option  */
   if (! formatting_specs.header_only &&
       group_wanted(ncid, formatting_specs.nlgrps, formatting_specs.grpids) ) {
      if (nvars > 0) {
	  indent_out();
	  printf ("data:\n");
      }
      for (varid = 0; varid < nvars; varid++) {
	 int no_data;
	 /* if var list specified, test for membership */
	 if (formatting_specs.nlvars > 0 && ! idmember(vlist, varid))
	    continue;
	 NC_CHECK( nc_inq_varndims(ncid, varid, &var.ndims) );
	 if(var.dims != NULL) {free(var.dims); var.dims = NULL;}
	 var.dims = (int *) emalloc((var.ndims + 1) * sizeof(int));
	 NC_CHECK( nc_inq_var(ncid, varid, var.name, &var.type, 0,
			      var.dims, &var.natts) );
	 var.tinfo = get_typeinfo(var.type);
	 /* If coords-only option specified, don't get data for
	  * non-coordinate vars */
	 if (formatting_specs.coord_vals && !iscoordvar(ncid,varid)) {
	    continue;
	 }
	 /* Collect variable's dim sizes */
	 if (vdims) {
	     free(vdims);
	     vdims = 0;
	 }
	 vdims = (size_t *) emalloc((var.ndims + 1) * SIZEOF_SIZE_T);
	 no_data = 0;
	 for (id = 0; id < var.ndims; id++) {
	     size_t len;
	     NC_CHECK( nc_inq_dimlen(ncid, var.dims[id], &len) );
	     if(len == 0) {
		 no_data = 1;
	     }
	     vdims[id] = len;
	 }
	 /* Don't get data for record variables if no records have
	  * been written yet */
	 if (no_data) {
	     free(vdims);
	     vdims = 0;
	     continue;
	 }
	 get_fill_info(ncid, varid, &var); /* sets has_fillval, fillvalp mmbrs */
	 if(var.timeinfo != NULL) {
	     if(var.timeinfo->units) free(var.timeinfo->units);
	     free(var.timeinfo);
	 }
	 get_timeinfo(ncid, varid, &var); /* sets has_timeval, timeinfo mmbrs */
	 /* printf format used to print each value */
	 var.fmt = get_fmt(ncid, varid, var.type);
	 var.locid = ncid;
	 set_tostring_func(&var);
	 if (vardata(&var, vdims, ncid, varid) == -1) {
	    error("can't output data for variable %s", var.name);
	    goto done;
	 }
	 if(var.fillvalp != NULL)
	     {NC_CHECK(nc_reclaim_data_all(ncid,var.tinfo->tid,var.fillvalp,1)); var.fillvalp = NULL;}
	 if(var.dims) {free(var.dims); var.dims = NULL;}
      }
      if (vdims) {
	  free(vdims);
	  vdims = 0;
      }
   }

#ifdef USE_NETCDF4
   /* For netCDF-4 compiles, check to see if the file has any
    * groups. If it does, this function is called recursively on each
    * of them. */
   {
      int g, numgrps, *ncids;
      char group_name[NC_MAX_NAME + 1];

      /* See how many groups there are. */
      NC_CHECK( nc_inq_grps(ncid, &numgrps, NULL) );

      /* Allocate memory to hold the list of group ids. */
      ncids = emalloc((numgrps + 1) * sizeof(int));

      /* Get the list of group ids. */
      NC_CHECK( nc_inq_grps(ncid, NULL, ncids) );

      /* Call this function for each group. */
      for (g = 0; g < numgrps; g++)
      {
	  NC_CHECK( nc_inq_grpname(ncids[g], group_name) );
	  printf ("\n");
	  indent_out();
/* 	    printf ("group: %s {\n", group_name); */
	  printf ("group: ");
	  print_name (group_name);
	  printf (" {\n");
	  indent_more();
	  do_ncdump_rec(ncids[g], NULL);
	  indent_out();
/* 	    printf ("} // group %s\n", group_name); */
	  printf ("} // group ");
	  print_name (group_name);
	  printf ("\n");
	  indent_less();
      }

      free(ncids);
   }
#endif /* USE_NETCDF4 */

done:
   if(var.dims != NULL) free(var.dims);
   if(var.fillvalp != NULL) {
	/* Release any data hanging off of fillvalp */
	nc_reclaim_data_all(ncid,var.tinfo->tid,var.fillvalp,1);
	var.fillvalp = NULL;
   }
   if(var.timeinfo != NULL) {
      if(var.timeinfo->units) free(var.timeinfo->units);
      free(var.timeinfo);
   }
   if (dims)
      free(dims);
   if (vlist)
      freeidlist(vlist);
}


static void
do_ncdump(int ncid, const char *path)
{
   char* esc_specname;
   /* output initial line */
   indent_init();
   indent_out();
   esc_specname=escaped_name(formatting_specs.name);
   printf ("netcdf %s {\n", esc_specname);
   free(esc_specname);
   do_ncdump_rec(ncid, path);
   indent_out();
   printf ("}\n");
}


static void
do_ncdumpx(int ncid, const char *path)
{
    int ndims;			/* number of dimensions */
    int nvars;			/* number of variables */
    int ngatts;			/* number of global attributes */
    int xdimid;			/* id of unlimited dimension */
    int dimid;			/* dimension id */
    int varid;			/* variable id */
    ncdim_t *dims;		/* dimensions */
    ncvar_t var;		/* variable */
    int ia;			/* attribute number */
    int iv;			/* variable number */
    idnode_t* vlist = NULL;     /* list for vars specified with -v option */

    /*
     * If any vars were specified with -v option, get list of associated
     * variable ids
     */
    if (formatting_specs.nlvars > 0) {
	vlist = newidlist();	/* list for vars specified with -v option */
	for (iv=0; iv < formatting_specs.nlvars; iv++) {
	    NC_CHECK( nc_inq_varid(ncid, formatting_specs.lvars[iv], &varid) );
	    idadd(vlist, varid);
	}
    }

    /* output initial line */
    pr_initx(ncid, path);

    /*
     * get number of dimensions, number of variables, number of global
     * atts, and dimension id of unlimited dimension, if any
     */
    /* TODO: print names with XML-ish escapes fopr special chars */
    NC_CHECK( nc_inq(ncid, &ndims, &nvars, &ngatts, &xdimid) );
    /* get dimension info */
    dims = (ncdim_t *) emalloc((ndims + 1) * sizeof(ncdim_t));
    for (dimid = 0; dimid < ndims; dimid++) {
	NC_CHECK( nc_inq_dim(ncid, dimid, dims[dimid].name, &dims[dimid].size) );
	if (dimid == xdimid)
  	  printf("  <dimension name=\"%s\" length=\"%d\" isUnlimited=\"true\" />\n",
		 dims[dimid].name, (int)dims[dimid].size);
	else
	  printf ("  <dimension name=\"%s\" length=\"%d\" />\n",
		  dims[dimid].name, (int)dims[dimid].size);
    }

    /* get global attributes */
    for (ia = 0; ia < ngatts; ia++)
	pr_attx(ncid, NC_GLOBAL, ia); /* print ia-th global attribute */

    /* get variable info, with variable attributes */
    memset((void*)&var,0,sizeof(var));
    for (varid = 0; varid < nvars; varid++) {
	NC_CHECK( nc_inq_varndims(ncid, varid, &var.ndims) );
	if(var.dims != NULL) free(var.dims);
	var.dims = (int *) emalloc((var.ndims + 1) * sizeof(int));
	NC_CHECK( nc_inq_var(ncid, varid, var.name, &var.type, 0,
			     var.dims, &var.natts) );
	printf ("  <variable name=\"%s\"", var.name);
	pr_shape(&var, dims);

	/* handle one-line variable elements that aren't containers
	   for attributes or data values, since they need to be
	   rendered as <variable ... /> instead of <variable ..>
	   ... </variable> */
	if (var.natts == 0) {
	    if (
		/* header-only specified */
		(formatting_specs.header_only) ||
		/* list of variables specified and this variable not in list */
		(formatting_specs.nlvars > 0 && !idmember(vlist, varid))	||
		/* coordinate vars only and this is not a coordinate variable */
		(formatting_specs.coord_vals && !iscoordvar(ncid, varid)) ||
		/* this is a record variable, but no records have been written */
		(isrecvar(ncid,varid) && dims[xdimid].size == 0)
		) {
		printf (" type=\"%s\" />\n", prim_type_name(var.type));
		continue;
	    }
	}

	/* else nest attributes values, data values in <variable> ... </variable> */
	printf (" type=\"%s\">\n", prim_type_name(var.type));

	/* get variable attributes */
	for (ia = 0; ia < var.natts; ia++) {
	    pr_attx(ncid, varid, ia); /* print ia-th attribute */
	}
	printf ("  </variable>\n");
    }

    printf ("</netcdf>\n");
    if (vlist)
	freeidlist(vlist);
    if(dims)
	free(dims);
    if(var.dims != NULL)
        free(var.dims);
}

/*
 * Extract the significant-digits specifiers from the (deprecated and
 * undocumented) -d argument on the command-line and update the
 * default data formats appropriately.  This only exists because an
 * old version of ncdump supported the "-d" flag which did not
 * override the C_format attributes (if any).
 */
static void
set_sigdigs(const char *optarg)
{
    char *ptr1 = 0;
    char *ptr2 = 0;
    int flt_digits = FLT_DIGITS; /* default floating-point digits */
    int dbl_digits = DBL_DIGITS; /* default double-precision digits */

    if (optarg != 0 && (int) strlen(optarg) > 0 && optarg[0] != ',')
        flt_digits = (int)strtol(optarg, &ptr1, 10);

    if (flt_digits < 1 || flt_digits > 20) {
	error("unreasonable value for float significant digits: %d",
	      flt_digits);
    }
    if (ptr1 && *ptr1 == ',') {
      dbl_digits = (int)strtol(ptr1+1, &ptr2, 10);
      if (ptr2 == ptr1+1 || dbl_digits < 1 || dbl_digits > 20) {
	  error("unreasonable value for double significant digits: %d",
		dbl_digits);
      }
    }
    set_formats(flt_digits, dbl_digits);
}


/*
 * Extract the significant-digits specifiers from the -p argument on the
 * command-line, set flags so we can override C_format attributes (if any),
 * and update the default data formats appropriately.
 */
static void
set_precision(const char *optarg)
{
    char *ptr1 = 0;
    char *ptr2 = 0;
    int flt_digits = FLT_DIGITS;	/* default floating-point digits */
    int dbl_digits = DBL_DIGITS;	/* default double-precision digits */

    if (optarg != 0 && (int) strlen(optarg) > 0 && optarg[0] != ',') {
        flt_digits = (int)strtol(optarg, &ptr1, 10);
	float_precision_specified = 1;
    }

    if (flt_digits < 1 || flt_digits > 20) {
	error("unreasonable value for float significant digits: %d",
	      flt_digits);
    }
    if (ptr1 && *ptr1 == ',') {
	dbl_digits = (int) strtol(ptr1+1, &ptr2, 10);
	double_precision_specified = 1;
	if (ptr2 == ptr1+1 || dbl_digits < 1 || dbl_digits > 20) {
	    error("unreasonable value for double significant digits: %d",
		  dbl_digits);
	}
    }
    set_formats(flt_digits, dbl_digits);
}


#ifdef USE_DAP
#define DAP_CLIENT_CACHE_DIRECTIVE	"cache"
/* replace path string with same string prefixed by
 * DAP_CLIENT_NCDUMP_DIRECTIVE */
static void
adapt_url_for_cache(char **pathp)
{
    char* path = *pathp;
    NCURI* url = NULL;
    ncuriparse(path,&url);
    if(url == NULL) return;
    ncuriappendfragmentkey(url,DAP_CLIENT_CACHE_DIRECTIVE,NULL);
    if(*pathp) free(*pathp);
    path = ncuribuild(url,NULL,NULL,NCURIALL);
    if(pathp) {*pathp = path; path = NULL;}
    ncurifree(url);
    nullfree(path);
    return;
}
#endif

int
main(int argc, char *argv[])
{
    int ncstat = NC_NOERR;
    int c;
    int i;
    int max_len = 80;		/* default maximum line length */
    int nameopt = 0;
    bool_t xml_out = false;    /* if true, output NcML instead of CDL */
    bool_t kind_out = false;	/* if true, just output kind of netCDF file */
    bool_t kind_out_extended = false;	/* output inq_format vs inq_format_extended */
    int Xp_flag = 0;    /* indicate that -Xp flag was set */
    int XF_flag = 0;    /* indicate that -XF flag was set */
    char* path = NULL;
    char errmsg[4096];

    errmsg[0] = '\0';

#if defined(_WIN32) || defined(msdos) || defined(WIN64)
    putenv("PRINTF_EXPONENT_DIGITS=2"); /* Enforce unix/linux style exponent formatting. */
#endif

    progname = argv[0];
    set_formats(FLT_DIGITS, DBL_DIGITS); /* default for float, double data */

    /* If the user called ncdump without arguments, print the usage
     * message and return peacefully. */
    if (argc <= 1)
    {
       usage();
       exit(EXIT_SUCCESS);
    }

    opterr = 1;
    while ((c = getopt(argc, argv, "b:cd:f:g:hikl:n:p:stv:xwFKL:X:")) != EOF)
      switch(c) {
	case 'h':		/* dump header only, no data */
	  formatting_specs.header_only = true;
	  break;
	case 'c':		/* header, data only for coordinate dims */
	  formatting_specs.coord_vals = true;
	  break;
	case 'n':		/*
				 * provide different name than derived from
				 * file name
				 */
	  formatting_specs.name = optarg;
	  nameopt = 1;
	  break;
	case 'b':		/* brief comments in data section */
	  formatting_specs.brief_data_cmnts = true;
	  switch (tolower((int)optarg[0])) {
	    case 'c':
	      formatting_specs.data_lang = LANG_C;
	      break;
	    case 'f':
	      formatting_specs.data_lang = LANG_F;
	      break;
	    default:
	      snprintf(errmsg,sizeof(errmsg),"invalid value for -b option: %s", optarg);
	      goto fail;
	  }
	  break;
	case 'f':		/* full comments in data section */
	  formatting_specs.full_data_cmnts = true;
	  switch (tolower((int)optarg[0])) {
	    case 'c':
	      formatting_specs.data_lang = LANG_C;
	      break;
	    case 'f':
	      formatting_specs.data_lang = LANG_F;
	      break;
	    default:
	      snprintf(errmsg,sizeof(errmsg),"invalid value for -f option: %s", optarg);
	      goto fail;
	  }
	  break;
	case 'l':		/* maximum line length */
	  max_len = (int) strtol(optarg, 0, 0);
	  if (max_len < 10) {
	      snprintf(errmsg,sizeof(errmsg),"unreasonably small line length specified: %d", max_len);
	      goto fail;
	  }
	  break;
	case 'v':		/* variable names */
	  /* make list of names of variables specified */
	  make_lvars (optarg, &formatting_specs.nlvars, &formatting_specs.lvars);
	  break;
	case 'g':		/* group names */
	  /* make list of names of groups specified */
	  make_lgrps (optarg, &formatting_specs.nlgrps, &formatting_specs.lgrps,
			&formatting_specs.grpids);
	  break;
	case 'd':		/* specify precision for floats (deprecated, undocumented) */
	  set_sigdigs(optarg);
	  break;
	case 'p':		/* specify precision for floats, overrides attribute specs */
	  set_precision(optarg);
	  break;
        case 'x':		/* XML output (NcML) */
	  xml_out = true;
	  break;
        case 'k':	        /* just output what kind of netCDF file */
	  kind_out = true;
	  break;
        case 'K':	        /* extended format info */
	  kind_out_extended = true;
	  break;
	case 't':		/* human-readable strings for date-time values */
	  formatting_specs.string_times = true;
	  formatting_specs.iso_separator = false;
	  break;
	case 'i':		/* human-readable strings for data-time values with 'T' separator */
	  formatting_specs.string_times = true;
	  formatting_specs.iso_separator = true;
	  break;
        case 's':	    /* output special (virtual) attributes for
			     * netCDF-4 files and variables, including
			     * _DeflateLevel, _Chunking, _Endianness,
			     * _Format, _Checksum, _NoFill */
	  formatting_specs.special_atts = true;
	  break;
        case 'w':		/* with client-side cache for DAP URLs */
	  formatting_specs.with_cache = true;
	  break;
        case 'X':		/* special options */
	  switch (tolower((int)optarg[0])) {
	    case 'm':
	      formatting_specs.xopt_inmemory = 1;
	      break;
	    case 'p': /* suppress the properties attribute */
	      Xp_flag = 1; /* record that this flag was set */
	      break;
	    case 'f': /* output the _FillValue attribute type */
	      XF_flag = 1; /* record that this flag was set */
	      break;
	    default:
	      snprintf(errmsg,sizeof(errmsg),"invalid value for -X option: %s", optarg);
	      goto fail;
	  }
	  break;
        case 'L':
#ifdef LOGGING
	  {
	  int level = atoi(optarg);
	  if(level >= 0)
	    nc_set_log_level(level);
	  }
#endif
	  ncsetloglevel(NCLOGNOTE);
	  break;
	case 'F':
	  formatting_specs.filter_atts = true;
	  break;
        case '?':
	  usage();
	  exit(EXIT_FAILURE);
      }

    /* Decide xopt_props */
    if(formatting_specs.special_atts && Xp_flag == 1)
        formatting_specs.xopt_props = 0;
    else if(formatting_specs.special_atts && Xp_flag == 0)
        formatting_specs.xopt_props = 1;
    else if(!formatting_specs.special_atts)
	formatting_specs.xopt_props = 0;
    else
	formatting_specs.xopt_props = 0;

    /* Decide xopt_filltype */
    formatting_specs.xopt_filltype = XF_flag;

    set_max_len(max_len);

    argc -= optind;
    argv += optind;

    /* If no file arguments left or more than one, print usage message. */
    if (argc != 1)
    {
       usage();
       exit(EXIT_FAILURE);
    }

    i = 0;

    init_epsilons();

    /* We need to look for escape characters because the argument
       may have come in via a shell script */
    path = NC_shellUnescape(argv[i]);
    if(path == NULL) {
	snprintf(errmsg,sizeof(errmsg),"out of memory un-escaping argument %s", argv[i]);
	goto fail;
    }

    if (!nameopt)
        formatting_specs.name = name_path(path);
    if (argc > 0) {
        int ncid;
	/* If path is a URL, do some fixups */
	if(nc__testurl(path, NULL)) {/* See if this is a url */
	    /*  Prefix with client-side directive to
             * make ncdump reasonably efficient */
#ifdef USE_DAP
	    if(formatting_specs.with_cache) { /* by default, don't use cache directive */
	        adapt_url_for_cache(&path);
	    }
#endif
	} /* else fall thru and treat like a file path */
        if(formatting_specs.xopt_inmemory) {
#if 0
		size_t size = 0;
		void* mem = NULL;
		ncstat = fileopen(path,&mem,&size);
		if(ncstat == NC_NOERR)
	            ncstat = nc_open_mem(path,NC_INMEMORY,size,mem,&ncid);
#else
	        ncstat = nc_open(path,NC_DISKLESS|NC_NOWRITE,&ncid);
#endif
	    } else /* just a file */
	        ncstat = nc_open(path, NC_NOWRITE, &ncid);
	    if (ncstat != NC_NOERR) goto fail;

	    NC_CHECK( nc_inq_format(ncid, &formatting_specs.nc_kind) );
	    NC_CHECK( nc_inq_format_extended(ncid,
                                             &formatting_specs.nc_extended,
                                             &formatting_specs.nc_mode) );
	    if (kind_out) {
		printf ("%s\n", kind_string(formatting_specs.nc_kind));
	    } else if (kind_out_extended) {
		printf ("%s\n", kind_string_extended(formatting_specs.nc_extended,formatting_specs.nc_mode));
	    } else {
		/* Initialize list of types. */
		init_types(ncid);
		/* Check if any vars in -v don't exist */
		if(missing_vars(ncid, formatting_specs.nlvars, formatting_specs.lvars)) {
		    snprintf(errmsg,sizeof(errmsg),"-v: non-existent variables");
		    goto fail;
		}
		if(formatting_specs.nlgrps > 0) {
		    if(formatting_specs.nc_kind != NC_FORMAT_NETCDF4)
			goto fail;
		    /* Check if any grps in -g don't exist */
		    if(grp_matches(ncid, formatting_specs.nlgrps, formatting_specs.lgrps, formatting_specs.grpids) == 0)
			goto fail;
		}
		if (xml_out) {
		    if(formatting_specs.nc_kind == NC_FORMAT_NETCDF4) {
			snprintf(errmsg,sizeof(errmsg),"NcML output (-x) currently only permitted for netCDF classic model");
			goto fail;
		    }
		    do_ncdumpx(ncid, path);
		} else {
		    do_ncdump(ncid, path);
		}
	    }
	    NC_CHECK( nc_close(ncid) );
    }
    nullfree(path) path = NULL;
    nc_finalize();
    exit(EXIT_SUCCESS);

fail: /* ncstat failures */
    path = (path?path:strdup("<unknown>"));
    if(ncstat && strlen(errmsg) == 0)
	snprintf(errmsg,sizeof(errmsg),"%s: %s", path, nc_strerror(ncstat));
    nullfree(path); path = NULL;
    if(strlen(errmsg) > 0)
	error("%s", errmsg);
    nc_finalize();
    exit(EXIT_FAILURE);
}

/* Helper function for searchgrouptreedim
   search a specified group for matching dimid.
*/
static int
searchgroupdim(int grp, int dimid)
{
    int i,ret = NC_NOERR;
    int nids;
    int* ids = NULL;

    /* Get all dimensions in parentid */
    if ((ret = nc_inq_dimids(grp, &nids, NULL, 0)))
	goto done;
    if (nids > 0) {
	if (!(ids = (int *)malloc((size_t)nids * sizeof(int))))
	    {ret = NC_ENOMEM; goto done;}
	if ((ret = nc_inq_dimids(grp, &nids, ids, 0)))
	    goto done;
	for(i = 0; i < nids; i++) {
	    if(ids[i] == dimid) goto done;
	}
    } else
        ret = NC_EBADDIM;

done:
    nullfree(ids);
    return ret;
}

/* Helper function for do_ncdump_rec
   search a tree of groups for a matching dimid
   using a breadth first queue. Return the
   immediately enclosing group.
*/
static int
searchgrouptreedim(int ncid, int dimid, int* parentidp)
{
    int i,ret = NC_NOERR;
    int nids;
    int* ids = NULL;
    NClist* queue = nclistnew();
    int gid;
    uintptr_t id;

    id = ncid;
    nclistpush(queue,(void*)id); /* prime the queue */
    while(nclistlength(queue) > 0) {
        id = (uintptr_t)nclistremove(queue,0);
	gid  = (int)id;
        switch (ret = searchgroupdim(gid,dimid)) {
	case NC_NOERR: /* found it */
	    if(parentidp) *parentidp = gid;
	    goto done;
	case NC_EBADDIM: /* not in this group; keep looking */
	    break;
	default: goto done;
	}
	/* Get subgroups of gid and push onto front of the queue (for breadth first) */
        if((ret = nc_inq_grps(gid,&nids,NULL)))
            goto done;
        if (!(ids = (int *)malloc((size_t)nids * sizeof(int))))
	    {ret = NC_ENOMEM; goto done;}
        if ((ret = nc_inq_grps(gid, &nids, ids)))
            goto done;
	/* push onto the end of the queue */
        for(i=0;i<nids;i++) {
	    id = ids[i];
	    nclistpush(queue,(void*)id);
	}
	free(ids); ids = NULL;
    }
    /* Not found */
    ret = NC_EBADDIM;

done:
    nclistfree(queue);
    nullfree(ids);
    return ret;
}
