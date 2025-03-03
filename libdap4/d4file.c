/*********************************************************************
 *   Copyright 2018, UCAR/Unidata
 *   See netcdf/COPYRIGHT file for copying and redistribution conditions.
 *********************************************************************/

#include "ncdispatch.h"
#include "ncd4dispatch.h"
#include "d4includes.h"
#include "d4read.h"
#include "d4curlfunctions.h"

#ifdef _MSC_VER
#include <process.h>
#include <direct.h>
#endif

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

/**************************************************/
/* Forward */

static int constrainable(NCURI*);
static void freeCurl(NCD4curl*);
static void freeInfo(NCD4INFO*);
static int fragmentcheck(NCD4INFO*, const char* key, const char* subkey);
static const char* getfragment(NCD4INFO* info, const char* key);
static const char* getquery(NCD4INFO* info, const char* key);
static int set_curl_properties(NCD4INFO*);
static int makesubstrate(NCD4INFO* d4info);
static void resetInfoforRead(NCD4INFO* d4info);

/**************************************************/
/* Constants */

static const char* checkseps = "+,:;";

/**************************************************/
int
NCD4_open(const char * path, int mode,
          int basepe, size_t *chunksizehintp,
          void *mpidata, const NC_Dispatch *dispatch, int ncid)
{
    int ret = NC_NOERR;
    NCD4INFO* d4info = NULL;
    const char* value;
    NC* nc;
    NCD4meta* meta = NULL;
    size_t len = 0;
    void* contents = NULL;

    if(path == NULL)
	return THROW(NC_EDAPURL);

    assert(dispatch != NULL);

    /* Find pointer to NC struct for this file. */
    ret = NC_check_id(ncid,&nc);
    if(ret != NC_NOERR) {goto done;}

    /* Setup our NC and NCDAPCOMMON state*/

    d4info = (NCD4INFO*)calloc(1,sizeof(NCD4INFO));
    if(d4info == NULL) {ret = NC_ENOMEM; goto done;}

    nc->dispatchdata = d4info;
    nc->int_ncid = nc__pseudofd(); /* create a unique id */
    d4info->controller = (NC*)nc;

    /* Parse url and params */
    if(ncuriparse(nc->path,&d4info->uri))
	{ret = NC_EDAPURL; goto done;}

    /* Load auth info from rc file */
    if((ret = NC_authsetup(&d4info->auth, d4info->uri)))
	goto done;
    NCD4_curl_protocols(d4info);

    if(!constrainable(d4info->uri))
	SETFLAG(d4info->controls.flags,NCF_UNCONSTRAINABLE);

    /* fail if we are unconstrainable but have constraints */
    if(FLAGSET(d4info->controls.flags,NCF_UNCONSTRAINABLE)) {
	if(d4info->uri != NULL) {
	    const char* ce = ncuriquerylookup(d4info->uri,DAP4CE); /* Look for dap4.ce */
	    if(ce != NULL) {
	        nclog(NCLOGWARN,"Attempt to constrain an unconstrainable data source: %s=%s",
		   DAP4CE,ce);
	        ret = THROW(NC_EDAPCONSTRAINT);
	        goto done;
	    }
	}
    }

    /* process control client fragment parameters */
    NCD4_applyclientfragmentcontrols(d4info);

    /* Use libsrc4 code (netcdf-4) for storing metadata */
    {
	char tmpname[NC_MAX_NAME];

        /* Create fake file name: exact name must be unique,
           but is otherwise irrelevant because we are using NC_DISKLESS
        */
	if(strlen(d4info->controls.substratename) > 0)
            snprintf(tmpname,sizeof(tmpname),"%s",d4info->controls.substratename);
	else
            snprintf(tmpname,sizeof(tmpname),"tmp_%d",nc->int_ncid);

	/* Compute the relevant names for the substrate file */
        d4info->substrate.filename = strdup(tmpname);
	if(d4info->substrate.filename == NULL)
            {ret = NC_ENOMEM; goto done;}
    }

    /* Turn on logging; only do this after oc_open*/
    if((value = ncurifragmentlookup(d4info->uri,"log")) != NULL) {
	ncloginit();
	ncsetloglevel(NCLOGNOTE);
    }

    /* Check env values */
    if(getenv("CURLOPT_VERBOSE") != NULL)
        d4info->auth->curlflags.verbose = 1;


    /* Setup a curl connection */
    {
        CURL* curl = NULL; /* curl handle*/
	d4info->curl = (NCD4curl*)calloc(1,sizeof(NCD4curl));
	if(d4info->curl == NULL)
	    {ret = NC_ENOMEM; goto done;}
	/* create the connection */
        if((ret=NCD4_curlopen(&curl))!= NC_NOERR) goto done;
	d4info->curl->curl = curl;
        /* Load misc rc properties */
        NCD4_get_rcproperties(d4info);
        if((ret=set_curl_properties(d4info))!= NC_NOERR) goto done;
        /* Set the one-time curl flags */
        if((ret=NCD4_set_flags_perlink(d4info))!= NC_NOERR) goto done;
#if 1 /* temporarily make per-link */
        if((ret=NCD4_set_flags_perfetch(d4info))!= NC_NOERR) goto done;
#endif
    }

    d4info->curl->packet = ncbytesnew();
    ncbytessetalloc(d4info->curl->packet,DFALTPACKETSIZE); /*initial reasonable size*/

    /* Reset the substrate */
    if((ret=makesubstrate(d4info))) goto done;

    /* Always start by reading the DMR only */
    /* reclaim substrate.metadata */
    resetInfoforRead(d4info);
    /* Rebuild metadata */
    if((d4info->substrate.metadata=NCD4_newmeta(d4info))==NULL)
	{ret = NC_ENOMEM; goto done;}

    if((ret=NCD4_readDMR(d4info, d4info->controls.flags.flags))) goto done;

    /* set serial.rawdata */
    len = ncbyteslength(d4info->curl->packet);
    contents = ncbytesextract(d4info->curl->packet);
    NCD4_attachraw(d4info->substrate.metadata, len, contents);

    /* process query parameters */
    NCD4_applyclientquerycontrols(d4info);

    meta = d4info->substrate.metadata;

    /* Infer the mode */
    if((ret=NCD4_infermode(meta))) goto done;

    /* Process the dmr part */
    if((ret=NCD4_dechunk(meta))) goto done;

#ifdef D4DUMPDMR
  {
    fprintf(stderr,"=============\n");
    fputs(d4info->substrate.metadata->serial.dmr,stderr);
    fprintf(stderr,"\n=============\n");
    fflush(stderr);
  }
#endif

    if((ret = NCD4_parse(d4info->substrate.metadata))) goto done;

#ifdef D4DEBUGMETA
  {
    fprintf(stderr,"\n/////////////\n");
    NCbytes* buf = ncbytesnew();
    NCD4_print(d4info->substrate.metadata,buf);
    ncbytesnull(buf);
    fputs(ncbytescontents(buf),stderr);
    ncbytesfree(buf);
    fprintf(stderr,"\n/////////////\n");
    fflush(stderr);
  }
#endif

    /* Build the substrate metadata */
    ret = NCD4_metabuild(d4info->substrate.metadata,d4info->substrate.metadata->ncid);
    if(ret != NC_NOERR && ret != NC_EVARSIZE) goto done;

done:
    if(ret) {
	freeInfo(d4info);
        nc->dispatchdata = NULL;
    }
    return THROW(ret);
}

int
NCD4_close(int ncid, void* ignore)
{
    int ret = NC_NOERR;
    NC* nc;
    NCD4INFO* d4info;
    int substrateid;

    ret = NC_check_id(ncid, (NC**)&nc);
    if(ret != NC_NOERR) goto done;
    d4info = (NCD4INFO*)nc->dispatchdata;
    substrateid = makenc4id(nc,ncid);

    /* We call abort rather than close to avoid trying to write anything,
       except if we are debugging
     */
    if(FLAGSET(d4info->controls.debugflags,NCF_DEBUG_COPY)) {
        /* Dump the data into the substrate */
	if((ret = NCD4_debugcopy(d4info)))
	    goto done;
        ret = nc_close(substrateid);
    } else {
        ret = nc_abort(substrateid);
    }

    freeInfo(d4info);

done:
    return THROW(ret);
}

int
NCD4_abort(int ncid)
{
    return NCD4_close(ncid,NULL);
}

/**************************************************/

/* Reclaim an NCD4INFO instance */
static void
freeInfo(NCD4INFO* d4info)
{
    if(d4info == NULL) return;
    d4info->controller = NULL; /* break link */
    nullfree(d4info->rawurltext);
    nullfree(d4info->urltext);
    ncurifree(d4info->uri);
    freeCurl(d4info->curl);
    nullfree(d4info->data.memory);
    nullfree(d4info->data.ondiskfilename);
    if(d4info->data.ondiskfile != NULL)
	fclose(d4info->data.ondiskfile);
    nullfree(d4info->fileproto.filename);
    if(d4info->substrate.realfile
	&& !FLAGSET(d4info->controls.debugflags,NCF_DEBUG_COPY)) {
	/* We used real file, so we need to delete the temp file
           unless we are debugging.
	   Assume caller has done nc_close|nc_abort on the ncid.
           Note that in theory, this should not be necessary since
           AFAIK the substrate file is still in def mode, and
           when aborted, it should be deleted. But that is not working
           for some reason, so we delete it ourselves.
	*/
	if(d4info->substrate.filename != NULL) {
	    unlink(d4info->substrate.filename);
	}
    }
    nullfree(d4info->substrate.filename); /* always reclaim */
    NCD4_reclaimMeta(d4info->substrate.metadata);
    NC_authfree(d4info->auth);
    nclistfree(d4info->blobs);
    free(d4info);
}

/* Reset NCD4INFO instance for new read request */
static void
resetInfoforRead(NCD4INFO* d4info)
{
    if(d4info == NULL) return;
    if(d4info->substrate.realfile
	&& !FLAGSET(d4info->controls.debugflags,NCF_DEBUG_COPY)) {
	/* We used real file, so we need to delete the temp file
           unless we are debugging.
	   Assume caller has done nc_close|nc_abort on the ncid.
           Note that in theory, this should not be necessary since
           AFAIK the substrate file is still in def mode, and
           when aborted, it should be deleted. But that is not working
           for some reason, so we delete it ourselves.
	*/
	if(d4info->substrate.filename != NULL) {
	    unlink(d4info->substrate.filename);
        }
    }
    NCD4_resetMeta(d4info->substrate.metadata);
    nullfree(d4info->substrate.metadata);
    d4info->substrate.metadata = NULL;
}

static void
freeCurl(NCD4curl* curl)
{
    if(curl == NULL) return;
    NCD4_curlclose(curl->curl);
    ncbytesfree(curl->packet);
    nullfree(curl->errdata.code);
    nullfree(curl->errdata.message);
    free(curl);
}

/* Define the set of protocols known to be constrainable */
static const char* constrainableprotocols[] = {"http", "https",NULL};

static int
constrainable(NCURI* durl)
{
   const char** protocol = constrainableprotocols;
   for(;*protocol;protocol++) {
	if(strcmp(durl->protocol,*protocol)==0)
	    return 1;
   }
   return 0;
}

/*
Set curl properties for link based on rc files etc.
*/
static int
set_curl_properties(NCD4INFO* d4info)
{
    int ret = NC_NOERR;

    if(d4info->auth->curlflags.useragent == NULL) {
	char* agent;
        size_t len = strlen(DFALTUSERAGENT) + strlen(VERSION);
	len++; /*strlcat nul*/
	agent = (char*)malloc(len+1);
	strncpy(agent,DFALTUSERAGENT,len);
	strlcat(agent,VERSION,len);
        d4info->auth->curlflags.useragent = agent;
    }

    /* Some servers (e.g. thredds and columbia) appear to require a place
       to put cookies in order for some security functions to work
    */
    if(d4info->auth->curlflags.cookiejar != NULL
       && strlen(d4info->auth->curlflags.cookiejar) == 0) {
	free(d4info->auth->curlflags.cookiejar);
	d4info->auth->curlflags.cookiejar = NULL;
    }

    if(d4info->auth->curlflags.cookiejar == NULL) {
	/* If no cookie file was defined, define a default */
        char* path = NULL;
        char* newpath = NULL;
        int len;
	errno = 0;
	NCglobalstate* globalstate = NC_getglobalstate();

	/* Create the unique cookie file name */
        len =
	  strlen(globalstate->tempdir)
	  + 1 /* '/' */
	  + strlen("ncd4cookies");
        path = (char*)malloc(len+1);
        if(path == NULL) return NC_ENOMEM;
	snprintf(path,len,"%s/nc4cookies",globalstate->tempdir);
	/* Create the unique cookie file name */
        newpath = NC_mktmp(path);
        free(path);
	if(newpath == NULL) {
	    fprintf(stderr,"Cannot create cookie file\n");
	    goto fail;
	}
	d4info->auth->curlflags.cookiejar = newpath;
	d4info->auth->curlflags.cookiejarcreated = 1;
	errno = 0;
    }
    assert(d4info->auth->curlflags.cookiejar != NULL);

    /* Make sure the cookie jar exists and can be read and written */
    {
	FILE* f = NULL;
	char* fname = d4info->auth->curlflags.cookiejar;
	/* See if the file exists already */
        f = NCfopen(fname,"r");
	if(f == NULL) {
	    /* Ok, create it */
	    f = NCfopen(fname,"w+");
	    if(f == NULL) {
	        fprintf(stderr,"Cookie file cannot be read and written: %s\n",fname);
	        {ret= NC_EPERM; goto fail;}
	    }
	} else { /* test if file can be written */
	    fclose(f);
	    f = NCfopen(fname,"r+");
	    if(f == NULL) {
	        fprintf(stderr,"Cookie file is cannot be written: %s\n",fname);
	        {ret = NC_EPERM; goto fail;}
	    }
	}
	if(f != NULL) fclose(f);
    }

    return THROW(ret);

fail:
    return THROW(ret);
}

void
NCD4_applyclientfragmentcontrols(NCD4INFO* info)
{
    const char* value;

    /* clear all flags */
    CLRFLAG(info->controls.flags,ALL_NCF_FLAGS);

    /* Turn on any default on flags */
    SETFLAG(info->controls.flags,DFALT_ON_FLAGS);

    /* Set these also */
    SETFLAG(info->controls.flags,(NCF_NC4|NCF_NCDAP));

    if(fragmentcheck(info,"show","fetch"))
	SETFLAG(info->controls.flags,NCF_SHOWFETCH);

    if(fragmentcheck(info,"translate","nc4"))
	info->controls.translation = NCD4_TRANSNC4;

    /* Look at the debug flags */
    if(fragmentcheck(info,"debug","copy"))
	SETFLAG(info->controls.debugflags,NCF_DEBUG_COPY); /* => close */

    value = getfragment(info,"substratename");
    if(value != NULL)
      strncpy(info->controls.substratename,value,(NC_MAX_NAME-1));

    value = getfragment(info,"hyrax");
    if(value != NULL) {
      info->data.checksumignore = 1; /* Assume checksum, but ignore */	    
    }

    info->controls.opaquesize = DFALTOPAQUESIZE;
    value = getfragment(info,"opaquesize");
    if(value != NULL) {
	long long len = 0;
	if(sscanf(value,"%lld",&len) != 1 || len == 0)
	    nclog(NCLOGWARN,"bad [opaquesize] tag: %s",value);
	else
	    info->controls.opaquesize = (size_t)len;
    }

    value = getfragment(info,"fillmismatch");
    if(value != NULL) {
	SETFLAG(info->controls.flags,NCF_FILLMISMATCH);
	CLRFLAG(info->controls.debugflags,NCF_FILLMISMATCH_FAIL);
    }
    value = getfragment(info,"nofillmismatch");
    if(value != NULL) {
	CLRFLAG(info->controls.debugflags,NCF_FILLMISMATCH);
	SETFLAG(info->controls.debugflags,NCF_FILLMISMATCH_FAIL);
    }
}

void
NCD4_applyclientquerycontrols(NCD4INFO* info)
{
    const char* value = getquery(info,DAP4CSUM);
    if(value == NULL) {
        info->data.querychecksumming = DEFAULT_CHECKSUM_STATE;
    } else {
        if(strcasecmp(value,"false")==0) {
	    info->data.querychecksumming = 0;
        } else if(strcasecmp(value,"true")==0) {
	    info->data.querychecksumming = 1;
        } else {
            nclog(NCLOGWARN,"Unknown checksum mode: %s ; using default",value);
    	    info->data.querychecksumming = DEFAULT_CHECKSUM_STATE;
	}
    }
}

/* Search for substring in value of param. If substring == NULL; then just
   check if fragment is defined.
*/
static int
fragmentcheck(NCD4INFO* info, const char* key, const char* subkey)
{
    const char* value;
    char* p;

    value = getfragment(info, key);
    if(value == NULL)
	return 0;
    if(subkey == NULL) return 1;
    p = strstr(value,subkey);
    if(p == NULL) return 0;
    p += strlen(subkey);
    if(*p != '\0' && strchr(checkseps,*p) == NULL) return 0;
    return 1;
}

/*
Given a fragment key, return its value or NULL if not defined.
*/
static const char*
getfragment(NCD4INFO* info, const char* key)
{
    const char* value;

    if(info == NULL || key == NULL) return NULL;
    if((value=ncurifragmentlookup(info->uri,key)) == NULL)
	return NULL;
    return value;
}

/*
Given a query key, return its value or NULL if not defined.
*/
static const char*
getquery(NCD4INFO* info, const char* key)
{
    const char* value;

    if(info == NULL || key == NULL) return NULL;
    if((value=ncuriquerylookup(info->uri,key)) == NULL)
	return NULL;
    return value;
}

/**************************************************/

static int
makesubstrate(NCD4INFO* d4info)
{
    int ret = NC_NOERR;
    int new = NC_NETCDF4;
    int old = 0;
    int ncid = 0;
    int ncflags = NC_NETCDF4|NC_CLOBBER;

    if(d4info->substrate.nc4id != 0) {
        /* reset the substrate */
        nc_abort(d4info->substrate.nc4id);
        d4info->substrate.nc4id = 0;
    }
    /* Create the hidden substrate netcdf file.
	We want this hidden file to always be NC_NETCDF4, so we need to
	force default format temporarily in case user changed it.
	Since diskless is enabled, create file in-memory.
	*/
    ncflags |= NC_DISKLESS;
    if(FLAGSET(d4info->controls.debugflags,NCF_DEBUG_COPY)) {
	/* Cause data to be dumped to real file */
	ncflags |= NC_WRITE;
	ncflags &= ~(NC_DISKLESS); /* use real file */
    }
    nc_set_default_format(new,&old); /* save and change */
    ret = nc_create(d4info->substrate.filename,ncflags,&ncid);
    nc_set_default_format(old,&new); /* restore */
    /* Avoid fill on the substrate */
    nc_set_fill(ncid,NC_NOFILL,NULL);
    d4info->substrate.nc4id = ncid;
    return THROW(ret);
}

/* This function breaks the abstraction, but is necessary for
   code that accesses the underlying netcdf-4 metadata objects.
   Used in: NC_reclaim_data[_all]
            NC_copy_data[_all]
*/

NC*
NCD4_get_substrate(NC* nc)
{
    NC* subnc = NULL;
    /* Iff nc->dispatch is the DAP4 dispatcher, then do a level of indirection */
    if(USED4INFO(nc)) {
        NCD4INFO* d4 = (NCD4INFO*)nc->dispatchdata;
        /* Find pointer to NC struct for this file. */
        (void)NC_check_id(d4->substrate.nc4id,&subnc);
    } else subnc = nc;
    return subnc;
}
