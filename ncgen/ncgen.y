/*********************************************************************
 *   Copyright 2018, UCAR/Unidata
 *   See netcdf/COPYRIGHT file for copying and redistribution conditions.
 *   $Id: ncgen.y,v 1.42 2010/05/18 21:32:46 dmh Exp $
 *********************************************************************/

/* yacc source for "ncgen", a netCDL parser and netCDF generator */

%define parse.error verbose

%{
/*
static char SccsId[] = "$Id: ncgen.y,v 1.42 2010/05/18 21:32:46 dmh Exp $";
*/
#include        "includes.h"
#include        "netcdf_aux.h"
#include        "ncgeny.h"
#include        "ncgen.h"
#ifdef USE_NETCDF4
#include        "netcdf_filter.h"
#endif

/* Following are in ncdump (for now)*/
/* Need some (unused) definitions to get it to compile */
#define ncatt_t void*
#define ncvar_t void
#include "nctime.h"

#undef GENLIB1

/* parser controls */
#define YY_NO_INPUT 1

/* True if string a equals string b*/
#ifndef NCSTREQ
#define NCSTREQ(a, b)     (*(a) == *(b) && strcmp((a), (b)) == 0)
#endif
#define VLENSIZE  (sizeof(nc_vlen_t))
#define MAXFLOATDIM 4294967295.0

/* mnemonics */
typedef enum Attrkind {ATTRVAR, ATTRGLOBAL, DONTKNOW} Attrkind;

#define ISCONST 1
#define ISLIST 0

typedef nc_vlen_t vlen_t;

/* We retain the old representation of the symbol list
   as a linked list.
*/
List* symlist;

/* Track rootgroup separately*/
Symbol* rootgroup;

/* Track the group sequence */
static List* groupstack;

/* Provide a separate sequence for accumulating values
   during the parse.
*/
static List* stack;

/* track homogeneity of types for data lists*/
static nc_type consttype;

/* Misc. */
static int stackbase;
static int stacklen;
static int count;
static int opaqueid; /* counter for opaque constants*/
static int arrayuid; /* counter for pseudo-array types*/

char* primtypenames[PRIMNO] = {
"nat",
"byte", "char", "short",
"int", "float", "double",
"ubyte", "ushort", "uint",
"int64", "uint64",
"string"
};

static int GLOBAL_SPECIAL = _NCPROPS_FLAG
                            | _ISNETCDF4_FLAG
                            | _SUPERBLOCK_FLAG
                            | _FORMAT_FLAG ;

/*Defined in ncgen.l*/
extern int lineno;              /* line number for error messages */
extern Bytebuffer* lextext;           /* name or string with escapes removed */

extern double double_val;       /* last double value read */
extern float float_val;         /* last float value read */
extern long long int64_val;         /* last int64 value read */
extern int int32_val;             /* last int32 value read */
extern short int16_val;         /* last short value read */
extern unsigned long long uint64_val;         /* last int64 value read */
extern unsigned int uint32_val;             /* last int32 value read */
extern unsigned short uint16_val;         /* last short value read */
extern char char_val;           /* last char value read */
extern signed char byte_val;    /* last byte value read */
extern unsigned char ubyte_val;    /* last byte value read */

/* Track definitions of dims, types, attributes, and vars*/
List* grpdefs;
List* dimdefs;
List* attdefs; /* variable-specific attributes*/
List* gattdefs; /* global attributes only*/
List* xattdefs; /* unknown attributes*/
List* typdefs;
List* vardefs;
List* tmp;

/* Forward */
static NCConstant* makeconstdata(nc_type);
static NCConstant* evaluate(Symbol* fcn, Datalist* arglist);
static NCConstant* makeenumconstref(Symbol*);
static void addtogroup(Symbol*);
static Symbol* currentgroup(void);
static Symbol* createrootgroup(const char*);
static Symbol* creategroup(Symbol*);
static int dupobjectcheck(nc_class,Symbol*);
static void setpathcurrent(Symbol* sym);
static Symbol* makeattribute(Symbol*,Symbol*,Symbol*,Datalist*,Attrkind);
static Symbol* makeprimitivetype(nc_type i);
static Symbol* makespecial(int tag, Symbol* vsym, Symbol* tsym, void* data, int isconst);
static int containsfills(Datalist* list);
static void vercheck(int ncid);
static long long extractint(NCConstant* con);
#ifdef USE_NETCDF4
static int parsefilterflag(const char* sdata0, Specialdata* special);
static int parsecodecsflag(const char* sdata0, Specialdata* special);
static Symbol* identkeyword(const Symbol*);

#ifdef GENDEBUG1
static void printfilters(int nfilters, NC_H5_Filterspec** filters);
#endif
#endif

int yylex(void);

#ifndef NO_STDARG
static void yyerror(const char *fmt, ...);
#else
static void yyerror(fmt,va_alist) const char* fmt; va_dcl;
#endif

/* Extern */
extern int lex_init(void);

%}

/* DECLARATIONS */

%union {
Symbol* sym;
unsigned long  size; /* allow for zero size to indicate e.g. UNLIMITED*/
long           mark; /* track indices into the sequence*/
int            nctype; /* for tracking attribute list type*/
Datalist*      datalist;
NCConstant*    constant;
}

%token <sym>
        NC_UNLIMITED_K /* keyword for unbounded record dimension */
        CHAR_K      /* keyword for char datatype */
        BYTE_K      /* keyword for byte datatype */
        SHORT_K     /* keyword for short datatype */
        INT_K       /* keyword for int datatype */
        FLOAT_K     /* keyword for float datatype */
        DOUBLE_K    /* keyword for double datatype */
        UBYTE_K     /* keyword for unsigned byte datatype */
        USHORT_K    /* keyword for unsigned short datatype */
        UINT_K      /* keyword for unsigned int datatype */
        INT64_K     /* keyword for long long datatype */
        UINT64_K    /* keyword for unsigned long long datatype */
        STRING_K    /* keyword for string datatype */
        IDENT       /* name for a dimension, variable, or attribute */
        TERMSTRING  /* terminal string */
        CHAR_CONST  /* char constant (not ever generated by ncgen.l) */
        BYTE_CONST  /* byte constant */
        SHORT_CONST /* short constant */
        INT_CONST   /* int constant */
        INT64_CONST   /* long long constant */
        UBYTE_CONST  /* unsigned byte constant */
        USHORT_CONST /* unsigned short constant */
        UINT_CONST   /* unsigned int  constant */
        UINT64_CONST   /* unsigned long long  constant */
        FLOAT_CONST /* float constant */
        DOUBLE_CONST/* double constant */
        DIMENSIONS  /* keyword starting dimensions section, if any */
        VARIABLES   /* keyword starting variables section, if any */
        NETCDF      /* keyword declaring netcdf name */
        DATA        /* keyword starting data section, if any */
        TYPES
	COMPOUND
        ENUM
        OPAQUE_ /* 'OPAQUE' apparently conflicts with HDF4 code */
        OPAQUESTRING    /* 0x<even number of hexdigits> */
        GROUP
	PATH            /* / or (/IDENT)+(.IDENT)? */
	FILLMARKER	/* "_" as opposed to the attribute */
	NIL             /* NIL */
        _FILLVALUE
        _FORMAT
        _STORAGE
        _CHUNKSIZES
        _DEFLATELEVEL
        _SHUFFLE
        _ENDIANNESS
        _NOFILL
        _FLETCHER32
	_NCPROPS
	_ISNETCDF4
	_SUPERBLOCK
	_FILTER
	_CODECS
        _QUANTIZEBG
        _QUANTIZEGBR
        _QUANTIZEBR
	DATASETID

%type <sym> ident typename primtype dimd varspec
	    attrdecl enumid path dimref fielddim fieldspec
	    varident
%type <sym> typeref
%type <sym> varref
%type <sym> ambiguous_ref
%type <mark> enumidlist fieldlist fields varlist dimspec dimlist field
	     fielddimspec fielddimlist
%type <constant> dataitem constdata constint conststring constbool
%type <constant> simpleconstant function econstref
%type <datalist> datalist intlist datalist1 datalist0 arglist


%start  ncdesc /* start symbol for grammar */

%%

/* RULES */

ncdesc: NETCDF
	datasetid
        rootgroup
        {if (error_count > 0) YYABORT;}
        ;

datasetid: DATASETID {createrootgroup(datasetname);};

rootgroup: '{'
           groupbody
           subgrouplist
           '}';

/* 2/3/08 - Allow group body with only attributes. (H/T John Storrs). */
groupbody:
		attrdecllist
                typesection     /* Type definitions */
                dimsection      /* dimension declarations */
                vasection       /* variable and attribute declarations */
                datasection     /* data for variables within the group */
                ;

subgrouplist: /*empty*/ | subgrouplist namedgroup;

namedgroup: GROUP ident '{'
            {
		Symbol* id = $2;
                markcdf4("Group specification");
		if(creategroup(id) == NULL)
                    yyerror("duplicate group declaration within parent group for %s",
                                id->name);
            }
            groupbody
            subgrouplist
            {listpop(groupstack);}
            '}'
	    attrdecllist
	    ;

typesection:    /* empty */
                | TYPES {}
		| TYPES typedecls
			{markcdf4("Type specification");}
                ;

typedecls: type_or_attr_decl | typedecls type_or_attr_decl ;

typename: ident
	    { /* Use when defining a type */
              $1->objectclass = NC_TYPE;
              if(dupobjectcheck(NC_TYPE,$1))
                    yyerror("duplicate type declaration for %s",
                            $1->name);
              listpush(typdefs,(void*)$1);
	    }
	  ;

type_or_attr_decl: typedecl {} | attrdecl ';' {} ;

typedecl:
	  enumdecl optsemicolon
	| compounddecl optsemicolon
	| vlendecl optsemicolon
	| opaquedecl optsemicolon
	;

optsemicolon: /*empty*/ | ';' ;


enumdecl: primtype ENUM typename
          '{' enumidlist '}'
              {
		int i;
                addtogroup($3); /* sets prefix*/
                $3->objectclass=NC_TYPE;
                $3->subclass=NC_ENUM;
                $3->typ.basetype=$1;
                $3->typ.size = $1->typ.size;
                $3->typ.alignment = $1->typ.alignment;
                stackbase=$5;
                stacklen=listlength(stack);
                $3->subnodes = listnew();
                /* Variety of field fixups*/
		/* 1. add in the enum values*/
		/* 2. make this type be their container*/
		/* 3. make constant names visible in the group*/
		/* 4. set field basetype to be same as enum basetype*/
                for(i=stackbase;i<stacklen;i++) {
                   Symbol* eid = (Symbol*)listget(stack,i);
		   assert(eid->subclass == NC_ECONST);
		   addtogroup(eid);
                   listpush($3->subnodes,(void*)eid);
                   eid->container = $3;
		   eid->typ.basetype = $3->typ.basetype;
                }
                listsetlength(stack,stackbase);/* remove stack nodes*/
              }
          ;

enumidlist:   enumid
		{$$=listlength(stack); listpush(stack,(void*)$1);}
	    | enumidlist ',' enumid
		{
		    int i;
		    $$=$1;
		    /* check for duplicates*/
		    stackbase=$1;
		    stacklen=listlength(stack);
		    for(i=stackbase;i<stacklen;i++) {
		      Symbol* elem = (Symbol*)listget(stack,i);
		      if(strcmp($3->name,elem->name)==0)
  	                yyerror("duplicate enum declaration for %s",
        	                 elem->name);
		    }
		    listpush(stack,(void*)$3);
		}
	    ;

enumid: ident '=' constint
        {
            $1->objectclass=NC_TYPE;
            $1->subclass=NC_ECONST;
            $1->typ.econst=$3;
	    $$=$1;
        }
        ;

opaquedecl: OPAQUE_ '(' INT_CONST ')' typename
                {
		    vercheck(NC_OPAQUE);
                    addtogroup($5); /*sets prefix*/
                    $5->objectclass=NC_TYPE;
                    $5->subclass=NC_OPAQUE;
                    $5->typ.typecode=NC_OPAQUE;
                    $5->typ.size=int32_val;
                    (void)ncaux_class_alignment(NC_OPAQUE,&$5->typ.alignment);
                }
            ;

vlendecl: typeref '(' '*' ')' typename
                {
                    Symbol* basetype = $1;
		    vercheck(NC_VLEN);
                    addtogroup($5); /*sets prefix*/
                    $5->objectclass=NC_TYPE;
                    $5->subclass=NC_VLEN;
                    $5->typ.basetype=basetype;
                    $5->typ.typecode=NC_VLEN;
                    $5->typ.size=VLENSIZE;
                    (void)ncaux_class_alignment(NC_VLEN,&$5->typ.alignment);
                }
          ;

compounddecl: COMPOUND typename '{' fields '}'
          {
	    int i,j;
	    vercheck(NC_COMPOUND);
            addtogroup($2);
	    /* check for duplicate field names*/
	    stackbase=$4;
	    stacklen=listlength(stack);
	    for(i=stackbase;i<stacklen;i++) {
	      Symbol* elem1 = (Symbol*)listget(stack,i);
	      for(j=i+1;j<stacklen;j++) {
	          Symbol* elem2 = (Symbol*)listget(stack,j);
	          if(strcmp(elem1->name,elem2->name)==0) {
	            yyerror("duplicate field declaration for %s",elem1->name);
		  }
	      }
	    }
	    $2->objectclass=NC_TYPE;
            $2->subclass=NC_COMPOUND;
            $2->typ.basetype=NULL;
            $2->typ.typecode=NC_COMPOUND;
	    $2->subnodes = listnew();
	    /* Add in the fields*/
	    for(i=stackbase;i<stacklen;i++) {
	        Symbol* fsym = (Symbol*)listget(stack,i);
		fsym->container = $2;
 	        listpush($2->subnodes,(void*)fsym);
	    }
	    listsetlength(stack,stackbase);/* remove stack nodes*/
          }
            ;


fields:   field ';' {$$=$1;}
	   | fields field ';' {$$=$1;}
	   ;

field: typeref fieldlist
        {
	    int i;
	    $$=$2;
	    stackbase=$2;
	    stacklen=listlength(stack);
	    /* process each field in the fieldlist*/
            for(i=stackbase;i<stacklen;i++) {
                Symbol* f = (Symbol*)listget(stack,i);
		f->typ.basetype = $1;
            }
        }
        ;

primtype:         CHAR_K  { $$ = primsymbols[NC_CHAR]; }
                | BYTE_K  { $$ = primsymbols[NC_BYTE]; }
                | SHORT_K { $$ = primsymbols[NC_SHORT]; }
                | INT_K   { $$ = primsymbols[NC_INT]; }
                | FLOAT_K { $$ = primsymbols[NC_FLOAT]; }
                | DOUBLE_K{ $$ = primsymbols[NC_DOUBLE]; }
                | UBYTE_K  { vercheck(NC_UBYTE); $$ = primsymbols[NC_UBYTE]; }
                | USHORT_K { vercheck(NC_USHORT); $$ = primsymbols[NC_USHORT]; }
                | UINT_K   { vercheck(NC_UINT); $$ = primsymbols[NC_UINT]; }
                | INT64_K   { vercheck(NC_INT64); $$ = primsymbols[NC_INT64]; }
                | UINT64_K   { vercheck(NC_UINT64); $$ = primsymbols[NC_UINT64]; }
                | STRING_K   { vercheck(NC_STRING); $$ = primsymbols[NC_STRING]; }
                ;

dimsection:     /* empty */
                | DIMENSIONS {}
		| DIMENSIONS dimdecls {}
                ;

dimdecls:       dim_or_attr_decl ';'
                | dimdecls dim_or_attr_decl ';'
                ;

dim_or_attr_decl: dimdeclist {} | attrdecl {} ;

dimdeclist:     dimdecl
                | dimdeclist ',' dimdecl
                ;

dimdecl:
	  dimd '=' constint
              {
		$1->dim.declsize = (size_t)extractint($3);
#ifdef GENDEBUG1
fprintf(stderr,"dimension: %s = %llu\n",$1->name,(unsigned long long)$1->dim.declsize);
#endif
		reclaimconstant($3);
	      }
        | dimd '=' NC_UNLIMITED_K
                   {
		        $1->dim.declsize = NC_UNLIMITED;
		        $1->dim.isunlimited = 1;
#ifdef GENDEBUG1
fprintf(stderr,"dimension: %s = UNLIMITED\n",$1->name);
#endif
		   }
                ;

dimd:           ident
                   {
                     $1->objectclass=NC_DIM;
                     if(dupobjectcheck(NC_DIM,$1))
                        yyerror( "Duplicate dimension declaration for %s",
                                $1->name);
		     addtogroup($1);
		     $$=$1;
		     listpush(dimdefs,(void*)$1);
                   }
                ;

vasection:      /* empty */
                | VARIABLES {}
                | VARIABLES vadecls {}
                ;

vadecls:        vadecl_or_attr ';'
                | vadecls vadecl_or_attr ';'
                ;

vadecl_or_attr: vardecl {} | attrdecl {} ;

vardecl:        typeref varlist
		{
		    int i;
		    stackbase=$2;
		    stacklen=listlength(stack);
		    /* process each variable in the varlist*/
	            for(i=stackbase;i<stacklen;i++) {
	                Symbol* sym = (Symbol*)listget(stack,i);
			sym->objectclass = NC_VAR;
		        if(dupobjectcheck(NC_VAR,sym)) {
                            yyerror("Duplicate variable declaration for %s",
                                    sym->name);
			} else {
		  	    sym->typ.basetype = $1;
	                    addtogroup(sym);
		            listpush(vardefs,(void*)sym);
			}
		    }
		    listsetlength(stack,stackbase);/* remove stack nodes*/
		}
                ;

varlist:      varspec
	        {$$=listlength(stack);
                 listpush(stack,(void*)$1);
		}
            | varlist ',' varspec
	        {$$=$1; listpush(stack,(void*)$3);}
            ;

varspec:        varident dimspec
                    {
		    int i;
		    Dimset dimset;
		    Symbol* var = $1; /* for debugging */
		    stacklen=listlength(stack);
		    stackbase=$2;
		    count = stacklen - stackbase;
		    if(count >= NC_MAX_VAR_DIMS) {
			yyerror("%s has too many dimensions",$1->name);
			count = NC_MAX_VAR_DIMS - 1;
			stacklen = stackbase + count;
		    }
  	            dimset.ndims = count;
		    /* extract the actual dimensions*/
		    if(dimset.ndims > 0) {
		        for(i=0;i<count;i++) {
			    Symbol* dsym = (Symbol*)listget(stack,stackbase+i);
			    dimset.dimsyms[i] = dsym;
			}
			var->typ.dimset = dimset;
		    }
		    var->typ.basetype = NULL; /* not yet known*/
                    var->objectclass=NC_VAR;
		    listsetlength(stack,stackbase);/* remove stack nodes*/
		    $$ = var;
		    }
                ;

dimspec:        /* empty */ {$$=listlength(stack);}
                | '(' dimlist ')' {$$=$2;}
                ;

dimlist:        dimref {$$=listlength(stack); listpush(stack,(void*)$1);}
                | dimlist ',' dimref
		    {$$=$1; listpush(stack,(void*)$3);}
                ;

dimref: path
            {Symbol* dimsym = $1;
		dimsym->objectclass = NC_DIM;
		/* Find the actual dimension*/
		dimsym = locate(dimsym);
		if(dimsym == NULL) {
		    derror("Undefined or forward referenced dimension: %s",$1->name);
		    YYABORT;
		}
		$$=dimsym;
	    }
	;

fieldlist:
	  fieldspec
	    {$$=listlength(stack);
             listpush(stack,(void*)$1);
	    }
	| fieldlist ',' fieldspec
	    {$$=$1; listpush(stack,(void*)$3);}
        ;

fieldspec:
	ident fielddimspec
	    {
		int i;
		Dimset dimset;
		stackbase=$2;
		stacklen=listlength(stack);
		count = stacklen - stackbase;
		if(count >= NC_MAX_VAR_DIMS) {
		    yyerror("%s has too many dimensions",$1->name);
		    count = NC_MAX_VAR_DIMS - 1;
		    stacklen = stackbase + count;
		}
  	        dimset.ndims = count;
		if(count > 0) {
		    /* extract the actual dimensions*/
		    for(i=0;i<count;i++) {
		        Symbol* dsym = (Symbol*)listget(stack,stackbase+i);
		        dimset.dimsyms[i] = dsym;
		    }
		    $1->typ.dimset = dimset;
		}
		$1->typ.basetype = NULL; /* not yet known*/
                $1->objectclass=NC_TYPE;
                $1->subclass=NC_FIELD;
		listsetlength(stack,stackbase);/* remove stack nodes*/
		$$ = $1;
	    }
	;

fielddimspec:        /* empty */ {$$=listlength(stack);}
                | '(' fielddimlist ')' {$$=$2;}
                ;

fielddimlist:
	  fielddim {$$=listlength(stack); listpush(stack,(void*)$1);}
	| fielddimlist ',' fielddim
	    {$$=$1; listpush(stack,(void*)$3);}
        ;

fielddim:
	  UINT_CONST
	    {  /* Anonymous integer dimension.
	         Can only occur in type definitions*/
	     char anon[32];
	     sprintf(anon,"const%u",uint32_val);
	     $$ = install(anon);
	     $$->objectclass = NC_DIM;
	     $$->dim.isconstant = 1;
	     $$->dim.declsize = uint32_val;
	    }
	| INT_CONST
	    {  /* Anonymous integer dimension.
	         Can only occur in type definitions*/
	     char anon[32];
	     if(int32_val <= 0) {
		derror("field dimension must be positive");
		YYABORT;
	     }
	     sprintf(anon,"const%d",int32_val);
	     $$ = install(anon);
	     $$->objectclass = NC_DIM;
	     $$->dim.isconstant = 1;
	     $$->dim.declsize = int32_val;
	    }
	;


/* Use this when referencing defined objects */

varref:
	ambiguous_ref
	    {Symbol* vsym = $1;
		if(vsym->objectclass != NC_VAR) {
		    derror("Undefined or forward referenced variable: %s",vsym->name);
		    YYABORT;
		}
		$$=vsym;
	    }
	  ;

typeref:
	ambiguous_ref
	    {Symbol* tsym = $1;
		if(tsym->objectclass != NC_TYPE) {
		    derror("Undefined or forward referenced type: %s",tsym->name);
		    YYABORT;
		}
		$$=tsym;
	    }
	;

ambiguous_ref:
	path
	    {Symbol* tvsym = $1; Symbol* sym;
		/* disambiguate*/
		tvsym->objectclass = NC_VAR;
		sym = locate(tvsym);
		if(sym == NULL) {
		    tvsym->objectclass = NC_TYPE;
		    sym = locate(tvsym);
		    if(tvsym == NULL) {
		        derror("Undefined or forward referenced name: %s",$1->name);
		        YYABORT;
		    } else tvsym = sym;
		} else tvsym = sym;
		if(tvsym == NULL) {
		    derror("Undefined name (line %d): %s",$1->lineno,$1->name);
		    YYABORT;
		}
		$$=tvsym;
	    }
	| primtype {$$=$1;}
	;

/* Use this for all attribute decls */
/* Global vs var-specific will be separated in makeattribute */

/* Watch out; this is left recursive */
attrdecllist: /*empty*/ {} | attrdecl ';' attrdecllist {} ;

attrdecl:
	  ':' _NCPROPS '=' conststring
	    {$$ = makespecial(_NCPROPS_FLAG,NULL,NULL,(void*)$4,ISCONST);}
	| ':' _ISNETCDF4 '=' constbool
	    {$$ = makespecial(_ISNETCDF4_FLAG,NULL,NULL,(void*)$4,ISCONST);}
	| ':' _SUPERBLOCK '=' constint
	    {$$ = makespecial(_SUPERBLOCK_FLAG,NULL,NULL,(void*)$4,ISCONST);}
	| ':' ident '=' datalist
	    { $$=makeattribute($2,NULL,NULL,$4,ATTRGLOBAL);}
	| typeref ambiguous_ref ':' ident '=' datalist
	    {Symbol* tsym = $1; Symbol* vsym = $2; Symbol* asym = $4;
		if(vsym->objectclass == NC_VAR) {
		    $$=makeattribute(asym,vsym,tsym,$6,ATTRVAR);
		} else {
		    derror("Doubly typed attribute: %s",asym->name);
		    YYABORT;
		}
	    }
	| ambiguous_ref ':' ident '=' datalist
	    {Symbol* sym = $1; Symbol* asym = $3;
		if(sym->objectclass == NC_VAR) {
		    $$=makeattribute(asym,sym,NULL,$5,ATTRVAR);
		} else if(sym->objectclass == NC_TYPE) {
		    $$=makeattribute(asym,NULL,sym,$5,ATTRGLOBAL);
		} else {
		    derror("Attribute prefix not a variable or type: %s",asym->name);
		    YYABORT;
		}
	    }
	| ambiguous_ref ':' _FILLVALUE '=' datalist
	    {$$ = makespecial(_FILLVALUE_FLAG,$1,NULL,(void*)$5,ISLIST);}
	| typeref ambiguous_ref ':' _FILLVALUE '=' datalist
	    {$$ = makespecial(_FILLVALUE_FLAG,$2,$1,(void*)$6,ISLIST);}
	| ambiguous_ref ':' _STORAGE '=' conststring
	    {$$ = makespecial(_STORAGE_FLAG,$1,NULL,(void*)$5,ISCONST);}
	| ambiguous_ref ':' _CHUNKSIZES '=' intlist
	    {$$ = makespecial(_CHUNKSIZES_FLAG,$1,NULL,(void*)$5,ISLIST);}
	| ambiguous_ref ':' _FLETCHER32 '=' constbool
	    {$$ = makespecial(_FLETCHER32_FLAG,$1,NULL,(void*)$5,ISCONST);}
	| ambiguous_ref ':' _DEFLATELEVEL '=' constint
	    {$$ = makespecial(_DEFLATE_FLAG,$1,NULL,(void*)$5,ISCONST);}
	| ambiguous_ref ':' _SHUFFLE '=' constbool
	    {$$ = makespecial(_SHUFFLE_FLAG,$1,NULL,(void*)$5,ISCONST);}
	| ambiguous_ref ':' _ENDIANNESS '=' conststring
	    {$$ = makespecial(_ENDIAN_FLAG,$1,NULL,(void*)$5,ISCONST);}
	| ambiguous_ref ':' _FILTER '=' conststring
	    {$$ = makespecial(_FILTER_FLAG,$1,NULL,(void*)$5,ISCONST);}
	| ambiguous_ref ':' _CODECS '=' conststring
	    {$$ = makespecial(_CODECS_FLAG,$1,NULL,(void*)$5,ISCONST);}
	| ambiguous_ref ':' _QUANTIZEBG '=' constint
	    {$$ = makespecial(_QUANTIZEBG_FLAG,$1,NULL,(void*)$5,ISCONST);}
	| ambiguous_ref ':' _QUANTIZEGBR '=' constint
	    {$$ = makespecial(_QUANTIZEGBR_FLAG,$1,NULL,(void*)$5,ISCONST);}
	| ambiguous_ref ':' _QUANTIZEBR '=' constint
	    {$$ = makespecial(_QUANTIZEBR_FLAG,$1,NULL,(void*)$5,ISCONST);}
	| ambiguous_ref ':' _NOFILL '=' constbool
	    {$$ = makespecial(_NOFILL_FLAG,$1,NULL,(void*)$5,ISCONST);}
	| ':' _FORMAT '=' conststring
	    {$$ = makespecial(_FORMAT_FLAG,NULL,NULL,(void*)$4,ISCONST);}
	;

path:
	  ident
	    {
	        $$=$1;
                $1->ref.is_ref=1;
                $1->is_prefixed=0;
                setpathcurrent($1);
	    }
	| PATH
	    {
	        $$=$1;
                $1->ref.is_ref=1;
                $1->is_prefixed=1;
	        /* path is set in ncgen.l*/
	    }
	;

datasection:    /* empty */
                | DATA {}
                | DATA datadecls {}
                ;

datadecls:      datadecl ';'
                | datadecls datadecl ';'
                ;

datadecl:       varref '=' datalist
                   {$1->data = $3;}
                ;
datalist:
	  datalist0 {$$ = $1;}
	| datalist1 {$$ = $1;}
	;

datalist0:
	/*empty*/ {$$ = builddatalist(0);}
	;

datalist1: /* Must have at least 1 element */
	  dataitem {$$ = const2list($1);}
	| datalist ',' dataitem
	    {dlappend($1,($3)); $$=$1; }
	;

dataitem:
	  constdata {$$=$1;}
	| '{' datalist '}' {$$=builddatasublist($2);}
	;

constdata:
	  simpleconstant      {$$=$1;}
	| OPAQUESTRING	{$$=makeconstdata(NC_OPAQUE);}
	| FILLMARKER	{$$=makeconstdata(NC_FILLVALUE);}
	| NIL   	{$$=makeconstdata(NC_NIL);}
	| econstref	{$$=$1;}
	| function
	;

econstref:
	path {$$ = makeenumconstref($1);}
	;

function:
	ident '(' arglist ')' {$$=evaluate($1,$3);}
	;

arglist:
	  simpleconstant
	    {$$ = const2list($1);}
	| arglist ',' simpleconstant
	    {dlappend($1,($3)); $$=$1;}
	;

simpleconstant:
	  CHAR_CONST	{$$=makeconstdata(NC_CHAR);} /* never used apparently*/
	| BYTE_CONST	{$$=makeconstdata(NC_BYTE);}
	| SHORT_CONST	{$$=makeconstdata(NC_SHORT);}
	| INT_CONST	{$$=makeconstdata(NC_INT);}
	| INT64_CONST	{$$=makeconstdata(NC_INT64);}
	| UBYTE_CONST	{$$=makeconstdata(NC_UBYTE);}
	| USHORT_CONST	{$$=makeconstdata(NC_USHORT);}
	| UINT_CONST	{$$=makeconstdata(NC_UINT);}
	| UINT64_CONST	{$$=makeconstdata(NC_UINT64);}
	| FLOAT_CONST	{$$=makeconstdata(NC_FLOAT);}
	| DOUBLE_CONST	{$$=makeconstdata(NC_DOUBLE);}
	| TERMSTRING	{$$=makeconstdata(NC_STRING);}
	;

intlist:
	  constint {$$ = const2list($1);}
	| intlist ',' constint {$$=$1; dlappend($1,($3));}
	;

constint:
	  INT_CONST
		{$$=makeconstdata(NC_INT);}
	| UINT_CONST
		{$$=makeconstdata(NC_UINT);}
	| INT64_CONST
		{$$=makeconstdata(NC_INT64);}
	| UINT64_CONST
		{$$=makeconstdata(NC_UINT64);}
	;

conststring:
	TERMSTRING	{$$=makeconstdata(NC_STRING);}
	;

constbool:
	  conststring {$$=$1;}
	| constint {$$=$1;}

/* End OF RULES */


/* Push all idents thru these*/

varident:
	  IDENT {$$=$1;}
	| DATA {$$=identkeyword($1);}
	;

ident:
	IDENT {$$=$1;}
	;

%%

#ifndef NO_STDARG
static void
yyerror(const char *fmt, ...)
#else
static void
yyerror(fmt,va_alist) const char* fmt; va_dcl
#endif
{
    va_list argv;
    va_start(argv,fmt);
    (void)fprintf(stderr,"%s: %s line %d: ", progname, cdlname, lineno);
    vderror(fmt,argv);
    va_end(argv);
}

/* undefine yywrap macro, in case we are using bison instead of yacc */
#ifdef yywrap
#undef yywrap
#endif

static int
ncgwrap(void)                    /* returns 1 on EOF if no more input */
{
    return  1;
}

/* get lexical input routine generated by lex  */
#include "ncgenl.c"

/* Really should init our data within this file */
void
parse_init(void)
{
    int i;
    opaqueid = 0;
    arrayuid = 0;
    symlist = listnew();
    stack = listnew();
    groupstack = listnew();
    consttype = NC_NAT;
    grpdefs = listnew();
    dimdefs = listnew();
    attdefs = listnew();
    gattdefs = listnew();
    xattdefs = listnew();
    typdefs = listnew();
    vardefs = listnew();
    tmp = listnew();
    /* Create the primitive types */
    for(i=NC_NAT+1;i<=NC_STRING;i++) {
        primsymbols[i] = makeprimitivetype(i);
    }
    lex_init();
}

static Symbol*
makeprimitivetype(nc_type nctype)
{
    Symbol* sym = install(primtypenames[nctype]);
    sym->objectclass=NC_TYPE;
    sym->subclass=NC_PRIM;
    sym->nc_id = nctype;
    sym->typ.typecode = nctype;
    sym->typ.size = ncsize(nctype);
    sym->typ.nelems = 1;
    (void)ncaux_class_alignment(nctype,&sym->typ.alignment);
    /* Make the basetype circular so we can always ask for it */
    sym->typ.basetype = sym;
    sym->prefix = listnew();
    return sym;
}

/* Symbol table operations for ncgen tool */
/* install sname in symbol table even if it is already there */
Symbol*
install(const char *sname)
{
    return installin(sname,currentgroup());
}

Symbol*
installin(const char *sname, Symbol* grp)
{
    Symbol* sp;
    sp = (Symbol*) ecalloc (sizeof (struct Symbol));
    sp->name = nulldup(sname);
    sp->lineno = lineno;
    sp->location = grp;
    sp->container = grp;
    listpush(symlist,sp);
    return sp;
}

static Symbol*
currentgroup(void)
{
    if(listlength(groupstack) == 0) return rootgroup;
    return (Symbol*)listtop(groupstack);
}

static Symbol*
createrootgroup(const char* dataset)
{
    Symbol* gsym = install(dataset);
    gsym->objectclass = NC_GRP;
    gsym->container = NULL;
    gsym->subnodes = listnew();
    gsym->grp.is_root = 1;
    gsym->prefix = listnew();
    listpush(grpdefs,(void*)gsym);
    rootgroup = gsym;
    return gsym;
}

static Symbol*
creategroup(Symbol * gsym)
{
    /* See if this group already exists in currentgroup */
    gsym->objectclass = NC_GRP;
    if(dupobjectcheck(NC_GRP,gsym)) {
        derror("Duplicate group name in same scope: %s",gsym->name);
	return NULL;
    }
    addtogroup(gsym);
    gsym->subnodes = listnew();
    listpush(groupstack,(void*)gsym);
    listpush(grpdefs,(void*)gsym);
    return gsym;
}

static NCConstant*
makeconstdata(nc_type nctype)
{
    NCConstant* con = nullconst();
    consttype = nctype;
    con->nctype = nctype;
    con->lineno = lineno;
    con->filled = 0;
    switch (nctype) {
	case NC_CHAR: con->value.charv = char_val; break;
        case NC_BYTE: con->value.int8v = byte_val; break;
        case NC_SHORT: con->value.int16v = int16_val; break;
        case NC_INT: con->value.int32v = int32_val; break;
        case NC_FLOAT:
	    con->value.floatv = float_val;
	    break;
        case NC_DOUBLE:
	    con->value.doublev = double_val;
	    break;
        case NC_STRING: { /* convert to a set of chars*/
	    size_t len;
	    len = bbLength(lextext);
	    con->value.stringv.len = len;
	    con->value.stringv.stringv = bbExtract(lextext);
	    }
	    break;

	/* Allow these constants even in netcdf-3 */
        case NC_UBYTE: con->value.uint8v = ubyte_val; break;
        case NC_USHORT: con->value.uint16v = uint16_val; break;
        case NC_UINT: con->value.uint32v = uint32_val; break;
        case NC_INT64: con->value.int64v = int64_val; break;
        case NC_UINT64: con->value.uint64v = uint64_val; break;

#ifdef USE_NETCDF4
	case NC_OPAQUE: {
	    char* s;
	    int len;
	    len = bbLength(lextext);
	    s = (char*)ecalloc(len+1);
	    strncpy(s,bbContents(lextext),len);
	    s[len] = '\0';
	    con->value.opaquev.stringv = s;
	    con->value.opaquev.len = len;
	    } break;

	case NC_NIL:
	    break; /* no associated value*/
#endif

 	case NC_FILLVALUE:
	    break; /* no associated value*/

	default:
	    yyerror("Data constant: unexpected NC type: %s",
		    nctypename(nctype));
	    con->value.stringv.stringv = NULL;
	    con->value.stringv.len = 0;
    }
    return con;
}

static NCConstant*
makeenumconstref(Symbol* refsym)
{
    NCConstant* con = nullconst();

    markcdf4("Enum type");
    consttype = NC_ENUM;
    con->nctype = NC_ECONST;
    con->lineno = lineno;
    con->filled = 0;
    refsym->objectclass = NC_TYPE;
    refsym->subclass = NC_ECONST;
    con->value.enumv = refsym;
    return con;
}

static void
addtogroup(Symbol* sym)
{
    Symbol* grp = currentgroup();
    sym->container = grp;
    listpush(grp->subnodes,(void*)sym);
    setpathcurrent(sym);
}

/* Check for duplicate name of given type within current group*/
static int
dupobjectcheck(nc_class objectclass, Symbol* pattern)
{
    int i;
    Symbol* grp;
    if(pattern == NULL) return 0;
    grp = pattern->container;
    if(grp == NULL || grp->subnodes == NULL) return 0;
    for(i=0;i<listlength(grp->subnodes);i++) {
	Symbol* sym = (Symbol*)listget(grp->subnodes,i);
	if(!sym->ref.is_ref && sym->objectclass == objectclass
	   && strcmp(sym->name,pattern->name)==0) return 1;
    }
    return 0;
}

static void
setpathcurrent(Symbol* sym)
{
    sym->is_prefixed = 0;
    sym->prefix = prefixdup(groupstack);
}

/* Convert an nc_type code to the corresponding Symbol*/
Symbol*
basetypefor(nc_type nctype)
{
    return primsymbols[nctype];
}

static int
truefalse(NCConstant* con, int tag)
{
    if(con->nctype == NC_STRING) {
	char* sdata = con->value.stringv.stringv;
	if(strncmp(sdata,"false",NC_MAX_NAME) == 0
           || strncmp(sdata,"0",NC_MAX_NAME) == 0)
	    return 0;
	else if(strncmp(sdata,"true",NC_MAX_NAME) == 0
           || strncmp(sdata,"1",NC_MAX_NAME) == 0)
	    return 1;
	else goto fail;
    } else if(con->value.int32v < 0 || con->value.int32v > 1)
	goto fail;
    return con->value.int32v;

fail:
    derror("%s: illegal value",specialname(tag));
    return 0;
}

/* Since this may be affected by the _Format attribute, which
   may come last, capture all the special info and sort it out
   in semantics.
*/
static Symbol*
makespecial(int tag, Symbol* vsym, Symbol* tsym, void* data, int isconst)
{
    Symbol* attr = NULL;
    Datalist* list = NULL;
    NCConstant* con = NULL;
    NCConstant* tmp = NULL;
    int tf = 0;
    char* sdata = NULL;
    int idata =  -1;

    if((GLOBAL_SPECIAL & tag) != 0) {
        if(vsym != NULL) {
            derror("_Format: must be global attribute");
            vsym = NULL;
        }
    } else {
        if(vsym == NULL) {
	    derror("%s: must have non-NULL vsym", specialname(tag));
	    return NULL;
        }
    }

    if(tag != _FILLVALUE_FLAG && tag != _FORMAT_FLAG)
        /*Main.*/specials_flag++;

    if(isconst)
	con = (NCConstant*)data;
    else
        list = (Datalist*)data;

    switch (tag) {
    case _FLETCHER32_FLAG:
    case _SHUFFLE_FLAG:
    case _ISNETCDF4_FLAG:
    case _NOFILL_FLAG:
	tmp = nullconst();
	tmp->nctype = (con->nctype == NC_STRING?NC_STRING:NC_INT);
	convert1(con,tmp);
	tf = truefalse(tmp,tag);
	reclaimconstant(tmp);
	break;
    case _FORMAT_FLAG:
    case _STORAGE_FLAG:
    case _NCPROPS_FLAG:
    case _ENDIAN_FLAG:
    case _FILTER_FLAG:
    case _CODECS_FLAG:
	tmp = nullconst();
        tmp->nctype = NC_STRING;
	convert1(con,tmp);
	if(tmp->nctype == NC_STRING) {
	    sdata = tmp->value.stringv.stringv;
	    tmp->value.stringv.stringv = NULL;
	    tmp->value.stringv.len = 0;
	} else
	    derror("%s: illegal value",specialname(tag));
	reclaimconstant(tmp);
	break;
    case _SUPERBLOCK_FLAG:
    case _DEFLATE_FLAG:
    case _QUANTIZEBG_FLAG:
    case _QUANTIZEGBR_FLAG:
    case _QUANTIZEBR_FLAG:
	tmp = nullconst();
        tmp->nctype = NC_INT;
	convert1(con,tmp);
	if(tmp->nctype == NC_INT)
	    idata = tmp->value.int32v;
	else
	    derror("%s: illegal value",specialname(tag));
	reclaimconstant(tmp);
	break;
    case _CHUNKSIZES_FLAG:
    case _FILLVALUE_FLAG:
	/* Handle below */
	break;
    default: PANIC1("unexpected special tag: %d",tag);
    }

    if(tag == _FORMAT_FLAG) {
	/* Watch out: this is a global attribute */
	struct Kvalues* kvalue;
	int found = 0;
	/* Use the table in main.c */
        for(kvalue = legalkinds; kvalue->name; kvalue++) {
          if(sdata) {
            if(strcmp(sdata, kvalue->name) == 0) {
              globalspecials._Format = kvalue->k_flag;
	      /*Main.*/format_attribute = 1;
              found = 1;
              break;
            }
          }
	}
	if(!found)
	    derror("_Format: illegal value: %s",sdata);
    } else if((GLOBAL_SPECIAL & tag) != 0) {
	if(tag == _ISNETCDF4_FLAG)
	    globalspecials._IsNetcdf4 = tf;
	else if(tag == _SUPERBLOCK_FLAG)
	    globalspecials._Superblock = idata;
	else if(tag == _NCPROPS_FLAG) {
	    globalspecials._NCProperties = sdata;
	    sdata = NULL;
	}
    } else {
        Specialdata* special;
        /* Set up special info */
        special = &vsym->var.special;
        if(tag == _FILLVALUE_FLAG) {
            /* fillvalue must be a single value*/
	    if(!isconst && datalistlen(list) != 1)
                derror("_FillValue: must be a single (possibly compound) value",
                            vsym->name);
	    if(isconst) {
	        list = const2list(con);
		con = NULL;
	    }
            /* check that the attribute value contains no fill values*/
            if(containsfills(list)) {
                derror("Attribute data may not contain fill values (i.e. _ )");
            }
            /* _FillValue is also a real attribute*/
            if(vsym->objectclass != NC_VAR) {
                derror("_FillValue attribute not associated with variable: %s",vsym->name);
            }
            if(tsym  == NULL) tsym = vsym->typ.basetype;
#if 0 /* No longer require matching types */
            else if(vsym->typ.basetype != tsym) {
                derror("_FillValue attribute type does not match variable type: %s",vsym->name);
            }
#endif
            special->_Fillvalue = clonedatalist(list);
	    /* Create the corresponding attribute */
            attr = makeattribute(install("_FillValue"),vsym,tsym,list,ATTRVAR);
	    list = NULL;
        } else switch (tag) {
	    /* These will be output as attributes later */
            case _STORAGE_FLAG:
              if(!sdata)
                derror("_Storage: illegal NULL value");
              else if(strcmp(sdata,"contiguous") == 0)
                special->_Storage = NC_CONTIGUOUS;
              else if(strcmp(sdata,"compact") == 0)
                special->_Storage = NC_COMPACT;
              else if(strcmp(sdata,"chunked") == 0)
                special->_Storage = NC_CHUNKED;
              else
                derror("_Storage: illegal value: %s",sdata);
              special->flags |= _STORAGE_FLAG;
              break;
          case _FLETCHER32_FLAG:
                special->_Fletcher32 = tf;
                special->flags |= _FLETCHER32_FLAG;
                break;
            case _DEFLATE_FLAG:
                special->_DeflateLevel = idata;
                special->flags |= _DEFLATE_FLAG;
                break;
            case _QUANTIZEBG_FLAG:
		special->_Quantizer = NC_QUANTIZE_BITGROOM;
                special->_NSD = idata;
                special->flags |= _QUANTIZEBG_FLAG;
                break;
            case _QUANTIZEGBR_FLAG:
		special->_Quantizer = NC_QUANTIZE_GRANULARBR;
                special->_NSD = idata;
                special->flags |= _QUANTIZEGBR_FLAG;
                break;
            case _QUANTIZEBR_FLAG:
		special->_Quantizer = NC_QUANTIZE_BITROUND;
                special->_NSD = idata;
                special->flags |= _QUANTIZEBR_FLAG;
                break;
            case _SHUFFLE_FLAG:
                special->_Shuffle = tf;
                special->flags |= _SHUFFLE_FLAG;
                break;
            case _ENDIAN_FLAG:
              if(!sdata)
                derror("_Endianness: illegal NULL value");
              else if(strcmp(sdata,"little") == 0)
                special->_Endianness = 1;
              else if(strcmp(sdata,"big") == 0)
                special->_Endianness = 2;
              else
                derror("_Endianness: illegal value: %s",sdata);
              special->flags |= _ENDIAN_FLAG;
              break;
          case _NOFILL_FLAG:
                special->_Fill = (1 - tf); /* negate */
                special->flags |= _NOFILL_FLAG;
                break;
          case _CHUNKSIZES_FLAG: {
                int i;
		list = (isconst ? const2list(con) : list);
                special->nchunks = list->length;
                special->_ChunkSizes = (size_t*)ecalloc(sizeof(size_t)*special->nchunks);
                for(i=0;i<special->nchunks;i++) {
		    tmp = nullconst();
                    tmp->nctype = NC_INT;
                    convert1(list->data[i],tmp);
                    if(tmp->nctype == NC_INT) {
                        special->_ChunkSizes[i] = (size_t)tmp->value.int32v;
                    } else {
                        efree(special->_ChunkSizes);
                        derror("%s: illegal value",specialname(tag));
                    }
		    reclaimconstant(tmp);
                }
                special->flags |= _CHUNKSIZES_FLAG;
                /* Chunksizes => storage == chunked */
                special->flags |= _STORAGE_FLAG;
                special->_Storage = NC_CHUNKED;
                } break;
          case _FILTER_FLAG:
#ifdef USE_NETCDF4
		/* Parse the filter spec */
		if(parsefilterflag(sdata,special) == NC_NOERR)
                    special->flags |= _FILTER_FLAG;
		else {
		    derror("_Filter: unparsable filter spec: %s",sdata);
		}
#else
	        derror("%s: the filter attribute requires netcdf-4 to be enabled",specialname(tag));
#endif
                break;
          case _CODECS_FLAG:
#ifdef USE_NETCDF4
		/* Parse the codec spec */
		if(parsecodecsflag(sdata,special) == NC_NOERR)
                    special->flags |= _CODECS_FLAG;
		else {
		    derror("_Codecs: unparsable codec spec: %s",sdata);
		}
#else
	        derror("%s: the _Codecs attribute requires netcdf-4 to be enabled",specialname(tag));
#endif
                break;
            default: PANIC1("makespecial: illegal token: %d",tag);
         }
    }
    if(sdata) efree(sdata);
    if(con) reclaimconstant(con);
    if(list) reclaimdatalist(list);
    return attr;
}

static Symbol*
makeattribute(Symbol* asym,
		Symbol* vsym,
		Symbol* tsym,
		Datalist* data,
		Attrkind kind) /* global var or unknown*/
{
    asym->objectclass = NC_ATT;
    asym->data = data;
    switch (kind) {
    case ATTRVAR:
        asym->att.var = vsym;
        asym->typ.basetype = tsym;
        listpush(attdefs,(void*)asym);
        addtogroup(asym);
	break;
    case ATTRGLOBAL:
        asym->att.var = NULL; /* NULL => NC_GLOBAL*/
        asym->typ.basetype = tsym;
        listpush(gattdefs,(void*)asym);
        addtogroup(asym);
	break;
    default: PANIC1("unexpected attribute type: %d",kind);
    }
    /* finally; check that the attribute value contains no fill values*/
    if(containsfills(data)) {
	derror("Attribute data may not contain fill values (i.e. _ ): %s",asym->name);
    }
    return asym;
}

static long long
extractint(NCConstant* con)
{
    switch (con->nctype) {
    case NC_BYTE: return (long long)(con->value.int8v);
    case NC_SHORT: return (long long)(con->value.int16v);
    case NC_INT: return (long long)(con->value.int32v);
    case NC_UBYTE: return (long long)(con->value.uint8v);
    case NC_USHORT: return (long long)(con->value.uint16v);
    case NC_UINT: return (long long)(con->value.uint32v);
    case NC_INT64: return (long long)(con->value.int64v);
    default:
	derror("Not a signed integer type: %d",con->nctype);
	break;
    }
    return 0;
}

static int
containsfills(Datalist* list)
{
    if(list != NULL) {
        int i;
        NCConstant** cons = list->data;
        for(i=0;i<list->length;i++) {
	    if(cons[i]->nctype == NC_COMPOUND) {
	        if(containsfills(cons[i]->value.compoundv)) return 1;
	    } else if(cons[i]->nctype == NC_FILLVALUE)
		return 1;
	}
    }
    return 0;
}

/*
Try to infer the file type from the
kinds of constructs used in the cdl file.
*/
static void
vercheck(int tid)
{
    switch (tid) {
    case NC_UBYTE: markcdf4("netCDF4/5 type: UBYTE"); break;
    case NC_USHORT: markcdf4("netCDF4/5 type: USHORT"); break;
    case NC_UINT: markcdf4("netCDF4/5 type: UINT"); break;
    case NC_INT64: markcdf4("netCDF4/5 type: INT64"); break;
    case NC_UINT64: markcdf4("netCDF4/5 type: UINT64"); break;
    case NC_STRING: markcdf4("netCDF4 type: STRING"); break;
    case NC_VLEN: markcdf4("netCDF4 type: VLEN"); break;
    case NC_OPAQUE: markcdf4("netCDF4 type: OPAQUE"); break;
    case NC_ENUM: markcdf4("netCDF4 type: ENUM"); break;
    case NC_COMPOUND: markcdf4("netCDF4 type: COMPOUND"); break;
    default: break;
    }
}

const char*
specialname(int tag)
{
    struct Specialtoken* spp = specials;
    for(;spp->name;spp++) {
	if(spp->tag == tag)
	    return spp->name;
    }
    return "<unknown>";
}

#ifdef USE_NETCDF4
/*
Parse a filter spec string and store it in special
*/
static int
parsefilterflag(const char* sdata, Specialdata* special)
{
    int stat = NC_NOERR;

    if(sdata == NULL || strlen(sdata) == 0) return NC_EINVAL;

    stat = ncaux_h5filterspec_parselist(sdata, NULL, &special->nfilters, &special->_Filters);
    if(stat)
        derror("Malformed filter spec: %s",sdata);
#ifdef GENDEBUG1
printfilters(special->nfilters,special->_Filters);
#endif
    return stat;
}

/*
Store a Codecs spec string in special
*/
static int
parsecodecsflag(const char* sdata, Specialdata* special)
{
    int stat = NC_NOERR;

    if(sdata == NULL || strlen(sdata) == 0) return NC_EINVAL;

    if((special->_Codecs = strdup(sdata))==NULL)
        return NC_ENOMEM;
    return stat;
}
#endif

/*
Since the arguments are all simple constants,
we can evaluate the function immediately
and return its value.
Note that currently, only a single value can
be returned.
*/

static NCConstant*
evaluate(Symbol* fcn, Datalist* arglist)
{
    NCConstant* result = nullconst();

    /* prepare the result */
    result->lineno = fcn->lineno;

    if(strcasecmp(fcn->name,"time") == 0) {
        char* timekind = NULL;
        char* timevalue = NULL;
        result->nctype = NC_DOUBLE;
        result->value.doublev = 0;
	/* int time([string],string) */
	switch (arglist->length) {
	case 2:
	    if(arglist->data[1]->nctype != NC_STRING) {
	        derror("Expected function signature: time([string,]string)");
	        goto done;
	    }
	    /* fall thru */
	case 1:
	    if(arglist->data[0]->nctype != NC_STRING) {
	        derror("Expected function signature: time([string,]string)");
	        goto done;
	    }
	    break;
	case 0:
	default:
	    derror("Expected function signature: time([string,]string)");
	    goto done;
	}
	if(arglist->length == 2) {
	    timekind = arglist->data[0]->value.stringv.stringv;
            timevalue = arglist->data[1]->value.stringv.stringv;
	} else
            timevalue = arglist->data[0]->value.stringv.stringv;
	if(timekind == NULL) { /* use cd time as the default */
            cdCompTime comptime;
	    CdTime cdtime;
	    cdCalenType timetype = cdStandard;
	    cdChar2Comp(timetype,timevalue,&comptime);
	    /* convert comptime to cdTime */
	    cdtime.year = comptime.year;
	    cdtime.month = comptime.month;
	    cdtime.day = comptime.day;
	    cdtime.hour = comptime.hour;
	    cdtime.baseYear = 1970;
	    cdtime.timeType = CdChron;
	    /* convert to double value */
	    Cdh2e(&cdtime,&result->value.doublev);
        } else {
	    derror("Time conversion '%s' not supported",timekind);
	    goto done;
	}
    } else {	/* Unknown function */
	derror("Unknown function name: %s",fcn->name);
	goto done;
    }

done:
    return result;
}

#ifdef GENDEBUG1
static void
printfilters(int nfilters, NC_H5_Filterspec** filters)
{
    int i;
    fprintf(stderr,"xxx: nfilters=%lu: ",(unsigned long)nfilters);
    for(i=0;i<nfilters;i++) {
	int k;
	NC_H5_Filterspec* sp = filters[i];
        fprintf(stderr,"{");
        fprintf(stderr,"filterid=%lu nparams=%lu params=%p",
		(unsigned long)sp->filterid,(unsigned long)sp->nparams,sp->params);
	if(sp->nparams > 0 && sp->params != NULL) {
            fprintf(stderr," params={");
            for(k=0;k<sp->nparams;k++) {
	        if(i==0) fprintf(stderr,",");
	        fprintf(stderr,"%u",sp->params[i]);
	    }
            fprintf(stderr,"}");
	} else
            fprintf(stderr,"params=NULL");
        fprintf(stderr,"}");
    }
    fprintf(stderr,"\n");
    fflush(stderr);
}
#endif
