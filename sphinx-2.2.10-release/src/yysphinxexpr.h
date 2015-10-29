/* A Bison parser, made by GNU Bison 1.875.  */

/* Skeleton parser for Yacc-like parsing with Bison,
   Copyright (C) 1984, 1989, 1990, 2000, 2001, 2002 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

/* As a special exception, when this file is copied by Bison into a
   Bison output file, you may use that output file without restriction.
   This special exception was added by the Free Software Foundation
   in version 1.24 of Bison.  */

/* Tokens.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
   /* Put the tokens into the symbol table, so that GDB and other debuggers
      know about them.  */
   enum yytokentype {
     TOK_CONST_INT = 258,
     TOK_CONST_FLOAT = 259,
     TOK_CONST_STRING = 260,
     TOK_SUBKEY = 261,
     TOK_DOT_NUMBER = 262,
     TOK_ATTR_INT = 263,
     TOK_ATTR_BITS = 264,
     TOK_ATTR_FLOAT = 265,
     TOK_ATTR_MVA32 = 266,
     TOK_ATTR_MVA64 = 267,
     TOK_ATTR_STRING = 268,
     TOK_ATTR_FACTORS = 269,
     TOK_FUNC = 270,
     TOK_FUNC_IN = 271,
     TOK_FUNC_REMAP = 272,
     TOK_FUNC_PF = 273,
     TOK_USERVAR = 274,
     TOK_UDF = 275,
     TOK_HOOK_IDENT = 276,
     TOK_HOOK_FUNC = 277,
     TOK_IDENT = 278,
     TOK_ATTR_JSON = 279,
     TOK_ATID = 280,
     TOK_ATWEIGHT = 281,
     TOK_ID = 282,
     TOK_GROUPBY = 283,
     TOK_WEIGHT = 284,
     TOK_COUNT = 285,
     TOK_DISTINCT = 286,
     TOK_CONST_LIST = 287,
     TOK_ATTR_SINT = 288,
     TOK_MAP_ARG = 289,
     TOK_FOR = 290,
     TOK_ITERATOR = 291,
     TOK_IS = 292,
     TOK_NULL = 293,
     TOK_IS_NULL = 294,
     TOK_IS_NOT_NULL = 295,
     TOK_OR = 296,
     TOK_AND = 297,
     TOK_NE = 298,
     TOK_EQ = 299,
     TOK_GTE = 300,
     TOK_LTE = 301,
     TOK_MOD = 302,
     TOK_DIV = 303,
     TOK_NOT = 304,
     TOK_NEG = 305
   };
#endif
#define TOK_CONST_INT 258
#define TOK_CONST_FLOAT 259
#define TOK_CONST_STRING 260
#define TOK_SUBKEY 261
#define TOK_DOT_NUMBER 262
#define TOK_ATTR_INT 263
#define TOK_ATTR_BITS 264
#define TOK_ATTR_FLOAT 265
#define TOK_ATTR_MVA32 266
#define TOK_ATTR_MVA64 267
#define TOK_ATTR_STRING 268
#define TOK_ATTR_FACTORS 269
#define TOK_FUNC 270
#define TOK_FUNC_IN 271
#define TOK_FUNC_REMAP 272
#define TOK_FUNC_PF 273
#define TOK_USERVAR 274
#define TOK_UDF 275
#define TOK_HOOK_IDENT 276
#define TOK_HOOK_FUNC 277
#define TOK_IDENT 278
#define TOK_ATTR_JSON 279
#define TOK_ATID 280
#define TOK_ATWEIGHT 281
#define TOK_ID 282
#define TOK_GROUPBY 283
#define TOK_WEIGHT 284
#define TOK_COUNT 285
#define TOK_DISTINCT 286
#define TOK_CONST_LIST 287
#define TOK_ATTR_SINT 288
#define TOK_MAP_ARG 289
#define TOK_FOR 290
#define TOK_ITERATOR 291
#define TOK_IS 292
#define TOK_NULL 293
#define TOK_IS_NULL 294
#define TOK_IS_NOT_NULL 295
#define TOK_OR 296
#define TOK_AND 297
#define TOK_NE 298
#define TOK_EQ 299
#define TOK_GTE 300
#define TOK_LTE 301
#define TOK_MOD 302
#define TOK_DIV 303
#define TOK_NOT 304
#define TOK_NEG 305




#if ! defined (YYSTYPE) && ! defined (YYSTYPE_IS_DECLARED)

typedef union YYSTYPE {
	int64_t			iConst;			// constant value
	float			fConst;			// constant value
	uint64_t		iAttrLocator;	// attribute locator (rowitem for int/float; offset+size for bits)
	int				iFunc;			// function id
	int				iNode;			// node, or uservar, or udf index
	const char *	sIdent;			// generic identifier (token does NOT own ident storage; ie values are managed by parser)
} YYSTYPE;
/* Line 1204 of yacc.c.  */

# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
# define YYSTYPE_IS_TRIVIAL 1
#endif





