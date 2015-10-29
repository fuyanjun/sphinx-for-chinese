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

/* Written by Richard Stallman by simplifying the original so called
   ``semantic'' parser.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output.  */
#define YYBISON 1

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 1

/* Using locations.  */
#define YYLSP_NEEDED 0



/* Tokens.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
   /* Put the tokens into the symbol table, so that GDB and other debuggers
      know about them.  */
   enum yytokentype {
     SEL_TOKEN = 258,
     SEL_ID = 259,
     SEL_AS = 260,
     SEL_AVG = 261,
     SEL_MAX = 262,
     SEL_MIN = 263,
     SEL_SUM = 264,
     SEL_GROUP_CONCAT = 265,
     SEL_GROUPBY = 266,
     SEL_COUNT = 267,
     SEL_WEIGHT = 268,
     SEL_DISTINCT = 269,
     SEL_OPTION = 270,
     SEL_COMMENT_OPEN = 271,
     SEL_COMMENT_CLOSE = 272,
     TOK_DIV = 273,
     TOK_MOD = 274,
     TOK_NEG = 275,
     TOK_LTE = 276,
     TOK_GTE = 277,
     TOK_EQ = 278,
     TOK_NE = 279,
     TOK_CONST_STRING = 280,
     TOK_OR = 281,
     TOK_AND = 282,
     TOK_NOT = 283,
     TOK_IS = 284,
     TOK_NULL = 285,
     TOK_FOR = 286,
     TOK_FUNC_IN = 287
   };
#endif
#define SEL_TOKEN 258
#define SEL_ID 259
#define SEL_AS 260
#define SEL_AVG 261
#define SEL_MAX 262
#define SEL_MIN 263
#define SEL_SUM 264
#define SEL_GROUP_CONCAT 265
#define SEL_GROUPBY 266
#define SEL_COUNT 267
#define SEL_WEIGHT 268
#define SEL_DISTINCT 269
#define SEL_OPTION 270
#define SEL_COMMENT_OPEN 271
#define SEL_COMMENT_CLOSE 272
#define TOK_DIV 273
#define TOK_MOD 274
#define TOK_NEG 275
#define TOK_LTE 276
#define TOK_GTE 277
#define TOK_EQ 278
#define TOK_NE 279
#define TOK_CONST_STRING 280
#define TOK_OR 281
#define TOK_AND 282
#define TOK_NOT 283
#define TOK_IS 284
#define TOK_NULL 285
#define TOK_FOR 286
#define TOK_FUNC_IN 287




/* Copy the first part of user declarations.  */


#if USE_WINDOWS
#pragma warning(push,1)
#pragma warning(disable:4702) // unreachable code
#endif


/* Enabling traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif

/* Enabling verbose error messages.  */
#ifdef YYERROR_VERBOSE
# undef YYERROR_VERBOSE
# define YYERROR_VERBOSE 1
#else
# define YYERROR_VERBOSE 1
#endif

#if ! defined (YYSTYPE) && ! defined (YYSTYPE_IS_DECLARED)
typedef int YYSTYPE;
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
# define YYSTYPE_IS_TRIVIAL 1
#endif



/* Copy the second part of user declarations.  */


/* Line 214 of yacc.c.  */


#if ! defined (yyoverflow) || YYERROR_VERBOSE

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# if YYSTACK_USE_ALLOCA
#  define YYSTACK_ALLOC alloca
# else
#  ifndef YYSTACK_USE_ALLOCA
#   if defined (alloca) || defined (_ALLOCA_H)
#    define YYSTACK_ALLOC alloca
#   else
#    ifdef __GNUC__
#     define YYSTACK_ALLOC __builtin_alloca
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's `empty if-body' warning. */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
# else
#  if defined (__STDC__) || defined (__cplusplus)
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   define YYSIZE_T size_t
#  endif
#  define YYSTACK_ALLOC malloc
#  define YYSTACK_FREE free
# endif
#endif /* ! defined (yyoverflow) || YYERROR_VERBOSE */


#if (! defined (yyoverflow) \
     && (! defined (__cplusplus) \
	 || (YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  short yyss;
  YYSTYPE yyvs;
  };

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (sizeof (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (sizeof (short) + sizeof (YYSTYPE))				\
      + YYSTACK_GAP_MAXIMUM)

/* Copy COUNT objects from FROM to TO.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if 1 < __GNUC__
#   define YYCOPY(To, From, Count) \
      __builtin_memcpy (To, From, (Count) * sizeof (*(From)))
#  else
#   define YYCOPY(To, From, Count)		\
      do					\
	{					\
	  register YYSIZE_T yyi;		\
	  for (yyi = 0; yyi < (Count); yyi++)	\
	    (To)[yyi] = (From)[yyi];		\
	}					\
      while (0)
#  endif
# endif

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack)					\
    do									\
      {									\
	YYSIZE_T yynewbytes;						\
	YYCOPY (&yyptr->Stack, Stack, yysize);				\
	Stack = &yyptr->Stack;						\
	yynewbytes = yystacksize * sizeof (*Stack) + YYSTACK_GAP_MAXIMUM; \
	yyptr += yynewbytes / sizeof (*yyptr);				\
      }									\
    while (0)

#endif

#if defined (__STDC__) || defined (__cplusplus)
   typedef signed char yysigned_char;
#else
   typedef short yysigned_char;
#endif

/* YYFINAL -- State number of the termination state. */
#define YYFINAL  75
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   818

/* YYNTOKENS -- Number of terminals. */
#define YYNTOKENS  51
/* YYNNTS -- Number of nonterminals. */
#define YYNNTS  20
/* YYNRULES -- Number of rules. */
#define YYNRULES  99
/* YYNRULES -- Number of states. */
#define YYNSTATES  193

/* YYTRANSLATE(YYLEX) -- Bison symbol number corresponding to YYLEX.  */
#define YYUNDEFTOK  2
#define YYMAXUTOK   287

#define YYTRANSLATE(YYX) 						\
  ((unsigned int) (YYX) <= YYMAXUTOK ? yytranslate[YYX] : YYUNDEFTOK)

/* YYTRANSLATE[YYLEX] -- Bison symbol number corresponding to YYLEX.  */
static const unsigned char yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,    36,    29,     2,
      43,    44,    34,    32,    42,    33,    48,    35,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
      30,     2,    31,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,    49,     2,    50,     2,     2,    45,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,    46,    28,    47,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    37,    38,    39,    40,    41
};

#if YYDEBUG
/* YYPRHS[YYN] -- Index of the first RHS symbol of rule number YYN in
   YYRHS.  */
static const unsigned short yyprhs[] =
{
       0,     0,     3,     5,     8,    10,    12,    16,    18,    21,
      22,    24,    27,    29,    34,    39,    44,    49,    54,    58,
      63,    69,    71,    75,    78,    81,    85,    89,    93,    97,
     101,   105,   109,   113,   117,   121,   125,   129,   133,   137,
     141,   145,   149,   153,   157,   159,   161,   165,   170,   174,
     176,   178,   183,   187,   194,   201,   205,   211,   216,   218,
     222,   226,   232,   234,   236,   238,   240,   247,   249,   251,
     254,   256,   259,   261,   263,   265,   267,   269,   271,   273,
     275,   277,   279,   281,   283,   285,   287,   289,   291,   293,
     295,   297,   299,   301,   303,   305,   307,   310,   314,   318
};

/* YYRHS -- A `-1'-separated list of the rules' RHS. */
static const yysigned_char yyrhs[] =
{
      52,     0,    -1,    53,    -1,    53,    64,    -1,    64,    -1,
      54,    -1,    53,    42,    54,    -1,    34,    -1,    56,    55,
      -1,    -1,     3,    -1,     5,     3,    -1,    57,    -1,     6,
      43,    57,    44,    -1,     7,    43,    57,    44,    -1,     8,
      43,    57,    44,    -1,     9,    43,    57,    44,    -1,    10,
      43,    57,    44,    -1,    11,    43,    44,    -1,    12,    43,
      34,    44,    -1,    12,    43,    14,     3,    44,    -1,    58,
      -1,    45,    58,    45,    -1,    33,    57,    -1,    37,    57,
      -1,    57,    32,    57,    -1,    57,    33,    57,    -1,    57,
      34,    57,    -1,    57,    35,    57,    -1,    57,    30,    57,
      -1,    57,    31,    57,    -1,    57,    29,    57,    -1,    57,
      28,    57,    -1,    57,    36,    57,    -1,    57,    18,    57,
      -1,    57,    19,    57,    -1,    57,    21,    57,    -1,    57,
      22,    57,    -1,    57,    23,    57,    -1,    57,    24,    57,
      -1,    57,    27,    57,    -1,    57,    26,    57,    -1,    43,
      57,    44,    -1,    46,    61,    47,    -1,    59,    -1,    66,
      -1,    65,    38,    39,    -1,    65,    38,    37,    39,    -1,
      57,    23,    25,    -1,     4,    -1,     3,    -1,     3,    43,
      60,    44,    -1,     3,    43,    44,    -1,     8,    43,    57,
      42,    57,    44,    -1,     7,    43,    57,    42,    57,    44,
      -1,    13,    43,    44,    -1,     3,    43,    57,    70,    44,
      -1,    41,    43,    60,    44,    -1,    63,    -1,    60,    42,
      63,    -1,    62,    23,     3,    -1,    61,    42,    62,    23,
       3,    -1,     3,    -1,    41,    -1,    57,    -1,    25,    -1,
      16,    15,     3,    23,     3,    17,    -1,    66,    -1,    68,
      -1,     3,    67,    -1,    69,    -1,    67,    69,    -1,     3,
      -1,     4,    -1,     5,    -1,     6,    -1,     7,    -1,     8,
      -1,     9,    -1,    10,    -1,    11,    -1,    12,    -1,    13,
      -1,    14,    -1,    15,    -1,    18,    -1,    19,    -1,    20,
      -1,    21,    -1,    22,    -1,    23,    -1,    24,    -1,    26,
      -1,    27,    -1,    37,    -1,    39,    -1,    48,    68,    -1,
      49,    57,    50,    -1,    49,    25,    50,    -1,    40,    68,
      41,    65,    -1
};

/* YYRLINE[YYN] -- source line where rule number YYN was defined.  */
static const unsigned char yyrline[] =
{
       0,    57,    57,    58,    59,    63,    64,    68,    69,    71,
      73,    74,    78,    79,    80,    81,    82,    83,    84,    85,
      86,    92,    93,    94,    95,    96,    97,    98,    99,   100,
     101,   102,   103,   104,   105,   106,   107,   108,   109,   110,
     111,   112,   113,   114,   115,   116,   117,   118,   119,   123,
     124,   128,   129,   130,   131,   132,   133,   134,   138,   139,
     143,   144,   148,   149,   153,   154,   158,   165,   166,   170,
     174,   175,   179,   180,   180,   180,   180,   180,   180,   180,
     181,   181,   181,   181,   181,   181,   182,   182,   182,   182,
     182,   182,   182,   182,   183,   183,   187,   188,   189,   193
};
#endif

#if YYDEBUG || YYERROR_VERBOSE
/* YYTNME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals. */
static const char *const yytname[] =
{
  "$end", "error", "$undefined", "SEL_TOKEN", "SEL_ID", "SEL_AS", "SEL_AVG", 
  "SEL_MAX", "SEL_MIN", "SEL_SUM", "SEL_GROUP_CONCAT", "SEL_GROUPBY", 
  "SEL_COUNT", "SEL_WEIGHT", "SEL_DISTINCT", "SEL_OPTION", 
  "SEL_COMMENT_OPEN", "SEL_COMMENT_CLOSE", "TOK_DIV", "TOK_MOD", 
  "TOK_NEG", "TOK_LTE", "TOK_GTE", "TOK_EQ", "TOK_NE", "TOK_CONST_STRING", 
  "TOK_OR", "TOK_AND", "'|'", "'&'", "'<'", "'>'", "'+'", "'-'", "'*'", 
  "'/'", "'%'", "TOK_NOT", "TOK_IS", "TOK_NULL", "TOK_FOR", "TOK_FUNC_IN", 
  "','", "'('", "')'", "'`'", "'{'", "'}'", "'.'", "'['", "']'", 
  "$accept", "select", "select_list", "select_item", "opt_alias", 
  "select_expr", "expr", "select_atom", "function", "arglist", 
  "consthash", "hash_key", "arg", "comment", "json_field", "json_expr", 
  "subscript", "ident", "subkey", "for_loop", 0
};
#endif

# ifdef YYPRINT
/* YYTOKNUM[YYLEX-NUM] -- Internal token number corresponding to
   token YYLEX-NUM.  */
static const unsigned short yytoknum[] =
{
       0,   256,   257,   258,   259,   260,   261,   262,   263,   264,
     265,   266,   267,   268,   269,   270,   271,   272,   273,   274,
     275,   276,   277,   278,   279,   280,   281,   282,   124,    38,
      60,    62,    43,    45,    42,    47,    37,   283,   284,   285,
     286,   287,    44,    40,    41,    96,   123,   125,    46,    91,
      93
};
# endif

/* YYR1[YYN] -- Symbol number of symbol that rule YYN derives.  */
static const unsigned char yyr1[] =
{
       0,    51,    52,    52,    52,    53,    53,    54,    54,    55,
      55,    55,    56,    56,    56,    56,    56,    56,    56,    56,
      56,    57,    57,    57,    57,    57,    57,    57,    57,    57,
      57,    57,    57,    57,    57,    57,    57,    57,    57,    57,
      57,    57,    57,    57,    57,    57,    57,    57,    57,    58,
      58,    59,    59,    59,    59,    59,    59,    59,    60,    60,
      61,    61,    62,    62,    63,    63,    64,    65,    65,    66,
      67,    67,    68,    68,    68,    68,    68,    68,    68,    68,
      68,    68,    68,    68,    68,    68,    68,    68,    68,    68,
      68,    68,    68,    68,    68,    68,    69,    69,    69,    70
};

/* YYR2[YYN] -- Number of symbols composing right hand side of rule YYN.  */
static const unsigned char yyr2[] =
{
       0,     2,     1,     2,     1,     1,     3,     1,     2,     0,
       1,     2,     1,     4,     4,     4,     4,     4,     3,     4,
       5,     1,     3,     2,     2,     3,     3,     3,     3,     3,
       3,     3,     3,     3,     3,     3,     3,     3,     3,     3,
       3,     3,     3,     3,     1,     1,     3,     4,     3,     1,
       1,     4,     3,     6,     6,     3,     5,     4,     1,     3,
       3,     5,     1,     1,     1,     1,     6,     1,     1,     2,
       1,     2,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     2,     3,     3,     4
};

/* YYDEFACT[STATE-NAME] -- Default rule to reduce with in state
   STATE-NUM when YYTABLE doesn't specify something else to do.  Zero
   means the default is an error.  */
static const unsigned char yydefact[] =
{
       0,    50,    49,    74,    75,    76,    77,    78,    79,    80,
      81,    82,    83,    84,     0,    85,    86,    87,    88,    89,
      90,    91,    92,    93,     0,     7,    94,    95,     0,     0,
       0,     0,     0,     2,     5,     9,    12,    21,    44,     4,
       0,    45,    68,     0,     0,     0,    69,    70,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    75,    76,    77,
      78,    79,    80,    81,    23,    24,     0,     0,    50,    49,
       0,    62,    63,     0,     0,     1,     0,     3,    10,     0,
       8,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    65,
      52,    64,     0,    58,    72,    73,    76,    77,    82,    94,
      96,     0,     0,    71,     0,     0,     0,     0,     0,    18,
       0,     0,    55,     0,     0,     0,    64,     0,    42,    22,
       0,    43,     0,     6,    11,    34,    35,    36,    37,    48,
      38,    39,    41,    40,    32,    31,    29,    30,    25,    26,
      27,    28,    33,     0,    46,     0,     0,     0,    51,    98,
      97,    13,     0,    14,     0,    15,    16,    17,     0,    19,
       0,     0,     0,    57,     0,    60,    47,     0,    56,    59,
       0,     0,    20,     0,     0,     0,    54,    53,    66,    61,
      72,    99,    67
};

/* YYDEFGOTO[NTERM-NUM]. */
static const short yydefgoto[] =
{
      -1,    32,    33,    34,    80,    35,    36,    37,    38,   102,
      73,    74,   103,    39,    40,    41,    46,    42,    47,   156
};

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
#define YYPACT_NINF -44
static const short yypact[] =
{
      72,   -31,   -15,   -44,    -7,    31,    46,    61,    67,    71,
      73,    76,   -44,   -44,    82,   -44,   -44,   -44,   -44,   -44,
     -44,   -44,   -44,   -44,   358,   -44,   358,   -44,    77,   358,
      10,     0,   126,   -12,   -44,     5,   718,   -44,   -44,   -44,
      90,    91,   -44,   138,   402,   182,    54,   -44,   358,   358,
     358,   358,   358,    83,    -5,    86,   128,   -44,    92,    93,
     -44,   -44,   -44,   -44,   -44,   -44,   226,   540,   -44,   -44,
      87,   -44,   -44,   -36,   111,   -44,   270,   -44,   -44,   134,
     -44,   358,   358,   358,   358,   314,   358,   358,   358,   358,
     358,   358,   358,   358,   358,   358,   358,   358,   -17,   -44,
     -44,   698,    11,   -44,   -44,   -44,   -44,   -44,   -44,   -44,
     -44,   104,   461,   -44,   559,   486,   513,   578,   597,   -44,
     136,   122,   -44,   132,   358,   358,   718,    12,   -44,   -44,
       0,   -44,   164,   -44,   -44,   -44,   -44,    89,    89,   -44,
     782,   782,    16,   737,   756,   775,    89,    89,    -3,    -3,
     -44,   -44,   -44,   129,   -44,   402,   125,   226,   -44,   -44,
     -44,   -44,   358,   -44,   358,   -44,   -44,   -44,   130,   -44,
     167,   654,   676,   -44,   149,   -44,   -44,   135,   -44,   -44,
     616,   635,   -44,   156,   175,   439,   -44,   -44,   -44,   -44,
      54,   -44,   -44
};

/* YYPGOTO[NTERM-NUM].  */
static const short yypgoto[] =
{
     -44,   -44,   -44,   123,   -44,   -44,   -24,   150,   -44,   144,
     -44,    68,    55,   178,    28,    29,   -44,   -43,   170,   -44
};

/* YYTABLE[YYPACT[STATE-NUM]].  What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule which
   number is the opposite.  If zero, do what YYDEFACT says.
   If YYTABLE_NINF, syntax error.  */
#define YYTABLE_NINF -74
static const short yytable[] =
{
      64,   110,    65,    71,    14,    67,   130,   -72,    78,   120,
      79,   131,    43,    68,    69,    81,    82,    44,    45,   101,
     153,   112,   154,   -73,   114,   115,   116,   117,   118,   121,
      76,    95,    96,    97,    81,    82,    48,    83,    84,    85,
      86,    72,   126,    88,    89,    90,    91,    92,    93,    94,
      95,    96,    97,   157,   157,   158,   173,   135,   136,   137,
     138,   140,   141,   142,   143,   144,   145,   146,   147,   148,
     149,   150,   151,   152,    49,     1,     2,     3,     4,     5,
       6,     7,     8,     9,    10,    11,    12,    13,    14,    50,
      15,    16,    17,    18,    19,    20,    21,    56,    22,    23,
     171,   172,    44,    45,    51,    24,    25,    81,    82,    26,
      52,    27,   177,    28,    53,    29,    54,    30,    31,    55,
      66,    93,    94,    95,    96,    97,    75,   119,    98,   -67,
     122,   123,   129,   126,   132,   124,   125,   134,   180,   168,
     181,     1,     2,     3,    57,    58,    59,    60,    61,    62,
      63,    11,    12,    13,   159,   170,    15,    16,    17,    18,
      19,    20,    21,    99,    22,    23,   169,   175,   176,   178,
     183,    24,   184,   188,   182,    26,   185,    27,   189,    28,
      70,    29,   100,    30,    31,     1,     2,     3,    57,    58,
      59,    60,    61,    62,    63,    11,    12,    13,   174,   133,
      15,    16,    17,    18,    19,    20,    21,   111,    22,    23,
     127,    77,   179,   191,   192,    24,   113,     0,     0,    26,
       0,    27,     0,    28,     0,    29,     0,    30,    31,     1,
       2,     3,    57,    58,    59,    60,    61,    62,    63,    11,
      12,    13,     0,     0,    15,    16,    17,    18,    19,    20,
      21,    99,    22,    23,     0,     0,     0,     0,     0,    24,
       0,     0,     0,    26,     0,    27,     0,    28,     0,    29,
       0,    30,    31,     1,     2,     3,     4,     5,     6,     7,
       8,     9,    10,    11,    12,    13,     0,     0,    15,    16,
      17,    18,    19,    20,    21,     0,    22,    23,     0,     0,
       0,     0,     0,    24,    25,     0,     0,    26,     0,    27,
       0,    28,     0,    29,     0,    30,    31,     1,     2,     3,
      57,    58,    59,    60,    61,    62,    63,    11,    12,    13,
       0,     0,    15,    16,    17,    18,    19,    20,    21,   139,
      22,    23,     0,     0,     0,     0,     0,    24,     0,     0,
       0,    26,     0,    27,     0,    28,     0,    29,     0,    30,
      31,     1,     2,     3,    57,    58,    59,    60,    61,    62,
      63,    11,    12,    13,     0,     0,    15,    16,    17,    18,
      19,    20,    21,     0,    22,    23,     0,     0,     0,     0,
       0,    24,     0,     0,     0,    26,     0,    27,     0,    28,
       0,    29,     0,    30,    31,   104,   105,     3,    57,   106,
     107,    60,    61,    62,    63,   108,    12,    13,     0,     0,
      15,    16,    17,    18,    19,    20,    21,     0,    22,    23,
       0,     0,     0,     0,     0,     0,     0,     0,     0,   109,
       0,    27,   190,   105,     3,    57,   106,   107,    60,    61,
      62,    63,   108,    12,    13,     0,     0,    15,    16,    17,
      18,    19,    20,    21,     0,    22,    23,     0,     0,     0,
       0,     0,     0,     0,     0,     0,   109,     0,    27,    81,
      82,     0,    83,    84,    85,    86,     0,    87,    88,    89,
      90,    91,    92,    93,    94,    95,    96,    97,     0,     0,
       0,     0,     0,     0,    81,    82,     0,    83,    84,    85,
      86,   160,    87,    88,    89,    90,    91,    92,    93,    94,
      95,    96,    97,     0,     0,     0,     0,     0,   162,     0,
     163,    81,    82,     0,    83,    84,    85,    86,     0,    87,
      88,    89,    90,    91,    92,    93,    94,    95,    96,    97,
       0,     0,     0,     0,     0,   164,     0,   165,    81,    82,
       0,    83,    84,    85,    86,     0,    87,    88,    89,    90,
      91,    92,    93,    94,    95,    96,    97,    81,    82,     0,
      83,    84,    85,    86,   128,    87,    88,    89,    90,    91,
      92,    93,    94,    95,    96,    97,    81,    82,     0,    83,
      84,    85,    86,   161,    87,    88,    89,    90,    91,    92,
      93,    94,    95,    96,    97,    81,    82,     0,    83,    84,
      85,    86,   166,    87,    88,    89,    90,    91,    92,    93,
      94,    95,    96,    97,    81,    82,     0,    83,    84,    85,
      86,   167,    87,    88,    89,    90,    91,    92,    93,    94,
      95,    96,    97,    81,    82,     0,    83,    84,    85,    86,
     186,    87,    88,    89,    90,    91,    92,    93,    94,    95,
      96,    97,    81,    82,     0,    83,    84,    85,    86,   187,
      87,    88,    89,    90,    91,    92,    93,    94,    95,    96,
      97,     0,     0,     0,    81,    82,   162,    83,    84,    85,
      86,     0,    87,    88,    89,    90,    91,    92,    93,    94,
      95,    96,    97,     0,     0,     0,    81,    82,   164,    83,
      84,    85,    86,     0,    87,    88,    89,    90,    91,    92,
      93,    94,    95,    96,    97,     0,    81,    82,   155,    83,
      84,    85,    86,     0,    87,    88,    89,    90,    91,    92,
      93,    94,    95,    96,    97,    81,    82,     0,    83,    84,
      85,    86,     0,     0,     0,    89,    90,    91,    92,    93,
      94,    95,    96,    97,    81,    82,     0,    83,    84,    85,
      86,     0,     0,     0,     0,    90,    91,    92,    93,    94,
      95,    96,    97,    81,    82,     0,    83,    84,    85,    86,
      81,    82,     0,    83,    84,    91,    92,    93,    94,    95,
      96,    97,    91,    92,    93,    94,    95,    96,    97
};

static const short yycheck[] =
{
      24,    44,    26,     3,    16,    29,    42,    38,     3,    14,
       5,    47,    43,     3,     4,    18,    19,    48,    49,    43,
      37,    45,    39,    38,    48,    49,    50,    51,    52,    34,
      42,    34,    35,    36,    18,    19,    43,    21,    22,    23,
      24,    41,    66,    27,    28,    29,    30,    31,    32,    33,
      34,    35,    36,    42,    42,    44,    44,    81,    82,    83,
      84,    85,    86,    87,    88,    89,    90,    91,    92,    93,
      94,    95,    96,    97,    43,     3,     4,     5,     6,     7,
       8,     9,    10,    11,    12,    13,    14,    15,    16,    43,
      18,    19,    20,    21,    22,    23,    24,    15,    26,    27,
     124,   125,    48,    49,    43,    33,    34,    18,    19,    37,
      43,    39,   155,    41,    43,    43,    43,    45,    46,    43,
      43,    32,    33,    34,    35,    36,     0,    44,    38,    38,
      44,     3,    45,   157,    23,    43,    43,     3,   162,     3,
     164,     3,     4,     5,     6,     7,     8,     9,    10,    11,
      12,    13,    14,    15,    50,    23,    18,    19,    20,    21,
      22,    23,    24,    25,    26,    27,    44,     3,    39,    44,
       3,    33,    23,    17,    44,    37,    41,    39,     3,    41,
      30,    43,    44,    45,    46,     3,     4,     5,     6,     7,
       8,     9,    10,    11,    12,    13,    14,    15,   130,    76,
      18,    19,    20,    21,    22,    23,    24,    25,    26,    27,
      66,    33,   157,   185,   185,    33,    46,    -1,    -1,    37,
      -1,    39,    -1,    41,    -1,    43,    -1,    45,    46,     3,
       4,     5,     6,     7,     8,     9,    10,    11,    12,    13,
      14,    15,    -1,    -1,    18,    19,    20,    21,    22,    23,
      24,    25,    26,    27,    -1,    -1,    -1,    -1,    -1,    33,
      -1,    -1,    -1,    37,    -1,    39,    -1,    41,    -1,    43,
      -1,    45,    46,     3,     4,     5,     6,     7,     8,     9,
      10,    11,    12,    13,    14,    15,    -1,    -1,    18,    19,
      20,    21,    22,    23,    24,    -1,    26,    27,    -1,    -1,
      -1,    -1,    -1,    33,    34,    -1,    -1,    37,    -1,    39,
      -1,    41,    -1,    43,    -1,    45,    46,     3,     4,     5,
       6,     7,     8,     9,    10,    11,    12,    13,    14,    15,
      -1,    -1,    18,    19,    20,    21,    22,    23,    24,    25,
      26,    27,    -1,    -1,    -1,    -1,    -1,    33,    -1,    -1,
      -1,    37,    -1,    39,    -1,    41,    -1,    43,    -1,    45,
      46,     3,     4,     5,     6,     7,     8,     9,    10,    11,
      12,    13,    14,    15,    -1,    -1,    18,    19,    20,    21,
      22,    23,    24,    -1,    26,    27,    -1,    -1,    -1,    -1,
      -1,    33,    -1,    -1,    -1,    37,    -1,    39,    -1,    41,
      -1,    43,    -1,    45,    46,     3,     4,     5,     6,     7,
       8,     9,    10,    11,    12,    13,    14,    15,    -1,    -1,
      18,    19,    20,    21,    22,    23,    24,    -1,    26,    27,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    37,
      -1,    39,     3,     4,     5,     6,     7,     8,     9,    10,
      11,    12,    13,    14,    15,    -1,    -1,    18,    19,    20,
      21,    22,    23,    24,    -1,    26,    27,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    37,    -1,    39,    18,
      19,    -1,    21,    22,    23,    24,    -1,    26,    27,    28,
      29,    30,    31,    32,    33,    34,    35,    36,    -1,    -1,
      -1,    -1,    -1,    -1,    18,    19,    -1,    21,    22,    23,
      24,    50,    26,    27,    28,    29,    30,    31,    32,    33,
      34,    35,    36,    -1,    -1,    -1,    -1,    -1,    42,    -1,
      44,    18,    19,    -1,    21,    22,    23,    24,    -1,    26,
      27,    28,    29,    30,    31,    32,    33,    34,    35,    36,
      -1,    -1,    -1,    -1,    -1,    42,    -1,    44,    18,    19,
      -1,    21,    22,    23,    24,    -1,    26,    27,    28,    29,
      30,    31,    32,    33,    34,    35,    36,    18,    19,    -1,
      21,    22,    23,    24,    44,    26,    27,    28,    29,    30,
      31,    32,    33,    34,    35,    36,    18,    19,    -1,    21,
      22,    23,    24,    44,    26,    27,    28,    29,    30,    31,
      32,    33,    34,    35,    36,    18,    19,    -1,    21,    22,
      23,    24,    44,    26,    27,    28,    29,    30,    31,    32,
      33,    34,    35,    36,    18,    19,    -1,    21,    22,    23,
      24,    44,    26,    27,    28,    29,    30,    31,    32,    33,
      34,    35,    36,    18,    19,    -1,    21,    22,    23,    24,
      44,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    18,    19,    -1,    21,    22,    23,    24,    44,
      26,    27,    28,    29,    30,    31,    32,    33,    34,    35,
      36,    -1,    -1,    -1,    18,    19,    42,    21,    22,    23,
      24,    -1,    26,    27,    28,    29,    30,    31,    32,    33,
      34,    35,    36,    -1,    -1,    -1,    18,    19,    42,    21,
      22,    23,    24,    -1,    26,    27,    28,    29,    30,    31,
      32,    33,    34,    35,    36,    -1,    18,    19,    40,    21,
      22,    23,    24,    -1,    26,    27,    28,    29,    30,    31,
      32,    33,    34,    35,    36,    18,    19,    -1,    21,    22,
      23,    24,    -1,    -1,    -1,    28,    29,    30,    31,    32,
      33,    34,    35,    36,    18,    19,    -1,    21,    22,    23,
      24,    -1,    -1,    -1,    -1,    29,    30,    31,    32,    33,
      34,    35,    36,    18,    19,    -1,    21,    22,    23,    24,
      18,    19,    -1,    21,    22,    30,    31,    32,    33,    34,
      35,    36,    30,    31,    32,    33,    34,    35,    36
};

/* YYSTOS[STATE-NUM] -- The (internal number of the) accessing
   symbol of state STATE-NUM.  */
static const unsigned char yystos[] =
{
       0,     3,     4,     5,     6,     7,     8,     9,    10,    11,
      12,    13,    14,    15,    16,    18,    19,    20,    21,    22,
      23,    24,    26,    27,    33,    34,    37,    39,    41,    43,
      45,    46,    52,    53,    54,    56,    57,    58,    59,    64,
      65,    66,    68,    43,    48,    49,    67,    69,    43,    43,
      43,    43,    43,    43,    43,    43,    15,     6,     7,     8,
       9,    10,    11,    12,    57,    57,    43,    57,     3,     4,
      58,     3,    41,    61,    62,     0,    42,    64,     3,     5,
      55,    18,    19,    21,    22,    23,    24,    26,    27,    28,
      29,    30,    31,    32,    33,    34,    35,    36,    38,    25,
      44,    57,    60,    63,     3,     4,     7,     8,    13,    37,
      68,    25,    57,    69,    57,    57,    57,    57,    57,    44,
      14,    34,    44,     3,    43,    43,    57,    60,    44,    45,
      42,    47,    23,    54,     3,    57,    57,    57,    57,    25,
      57,    57,    57,    57,    57,    57,    57,    57,    57,    57,
      57,    57,    57,    37,    39,    40,    70,    42,    44,    50,
      50,    44,    42,    44,    42,    44,    44,    44,     3,    44,
      23,    57,    57,    44,    62,     3,    39,    68,    44,    63,
      57,    57,    44,     3,    23,    41,    44,    44,    17,     3,
       3,    65,    66
};

#if ! defined (YYSIZE_T) && defined (__SIZE_TYPE__)
# define YYSIZE_T __SIZE_TYPE__
#endif
#if ! defined (YYSIZE_T) && defined (size_t)
# define YYSIZE_T size_t
#endif
#if ! defined (YYSIZE_T)
# if defined (__STDC__) || defined (__cplusplus)
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# endif
#endif
#if ! defined (YYSIZE_T)
# define YYSIZE_T unsigned int
#endif

#define yyerrok		(yyerrstatus = 0)
#define yyclearin	(yychar = YYEMPTY)
#define YYEMPTY		(-2)
#define YYEOF		0

#define YYACCEPT	goto yyacceptlab
#define YYABORT		goto yyabortlab
#define YYERROR		goto yyerrlab1

/* Like YYERROR except do call yyerror.  This remains here temporarily
   to ease the transition to the new meaning of YYERROR, for GCC.
   Once GCC version 2 has supplanted version 1, this can go.  */

#define YYFAIL		goto yyerrlab

#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)					\
do								\
  if (yychar == YYEMPTY && yylen == 1)				\
    {								\
      yychar = (Token);						\
      yylval = (Value);						\
      yytoken = YYTRANSLATE (yychar);				\
      YYPOPSTACK;						\
      goto yybackup;						\
    }								\
  else								\
    { 								\
      yyerror (pParser, "syntax error: cannot back up");\
      YYERROR;							\
    }								\
while (0)

#define YYTERROR	1
#define YYERRCODE	256

/* YYLLOC_DEFAULT -- Compute the default location (before the actions
   are run).  */

#ifndef YYLLOC_DEFAULT
# define YYLLOC_DEFAULT(Current, Rhs, N)         \
  Current.first_line   = Rhs[1].first_line;      \
  Current.first_column = Rhs[1].first_column;    \
  Current.last_line    = Rhs[N].last_line;       \
  Current.last_column  = Rhs[N].last_column;
#endif

/* YYLEX -- calling `yylex' with the right arguments.  */

#ifdef YYLEX_PARAM
# define YYLEX yylex (&yylval, YYLEX_PARAM)
#else
# define YYLEX yylex (&yylval, pParser)
#endif

/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)			\
do {						\
  if (yydebug)					\
    YYFPRINTF Args;				\
} while (0)

# define YYDSYMPRINT(Args)			\
do {						\
  if (yydebug)					\
    yysymprint Args;				\
} while (0)

# define YYDSYMPRINTF(Title, Token, Value, Location)		\
do {								\
  if (yydebug)							\
    {								\
      YYFPRINTF (stderr, "%s ", Title);				\
      yysymprint (stderr, 					\
                  Token, Value);	\
      YYFPRINTF (stderr, "\n");					\
    }								\
} while (0)

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (cinluded).                                                   |
`------------------------------------------------------------------*/

#if defined (__STDC__) || defined (__cplusplus)
static void
yy_stack_print (short *bottom, short *top)
#else
static void
yy_stack_print (bottom, top)
    short *bottom;
    short *top;
#endif
{
  YYFPRINTF (stderr, "Stack now");
  for (/* Nothing. */; bottom <= top; ++bottom)
    YYFPRINTF (stderr, " %d", *bottom);
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)				\
do {								\
  if (yydebug)							\
    yy_stack_print ((Bottom), (Top));				\
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

#if defined (__STDC__) || defined (__cplusplus)
static void
yy_reduce_print (int yyrule)
#else
static void
yy_reduce_print (yyrule)
    int yyrule;
#endif
{
  int yyi;
  unsigned int yylineno = yyrline[yyrule];
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %u), ",
             yyrule - 1, yylineno);
  /* Print the symbols being reduced, and their result.  */
  for (yyi = yyprhs[yyrule]; 0 <= yyrhs[yyi]; yyi++)
    YYFPRINTF (stderr, "%s ", yytname [yyrhs[yyi]]);
  YYFPRINTF (stderr, "-> %s\n", yytname [yyr1[yyrule]]);
}

# define YY_REDUCE_PRINT(Rule)		\
do {					\
  if (yydebug)				\
    yy_reduce_print (Rule);		\
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args)
# define YYDSYMPRINT(Args)
# define YYDSYMPRINTF(Title, Token, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef	YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   SIZE_MAX < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#if YYMAXDEPTH == 0
# undef YYMAXDEPTH
#endif

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif



#if YYERROR_VERBOSE

# ifndef yystrlen
#  if defined (__GLIBC__) && defined (_STRING_H)
#   define yystrlen strlen
#  else
/* Return the length of YYSTR.  */
static YYSIZE_T
#   if defined (__STDC__) || defined (__cplusplus)
yystrlen (const char *yystr)
#   else
yystrlen (yystr)
     const char *yystr;
#   endif
{
  register const char *yys = yystr;

  while (*yys++ != '\0')
    continue;

  return yys - yystr - 1;
}
#  endif
# endif

# ifndef yystpcpy
#  if defined (__GLIBC__) && defined (_STRING_H) && defined (_GNU_SOURCE)
#   define yystpcpy stpcpy
#  else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
static char *
#   if defined (__STDC__) || defined (__cplusplus)
yystpcpy (char *yydest, const char *yysrc)
#   else
yystpcpy (yydest, yysrc)
     char *yydest;
     const char *yysrc;
#   endif
{
  register char *yyd = yydest;
  register const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
#  endif
# endif

#endif /* !YYERROR_VERBOSE */



#if YYDEBUG
/*--------------------------------.
| Print this symbol on YYOUTPUT.  |
`--------------------------------*/

#if defined (__STDC__) || defined (__cplusplus)
static void
yysymprint (FILE *yyoutput, int yytype, YYSTYPE *yyvaluep)
#else
static void
yysymprint (yyoutput, yytype, yyvaluep)
    FILE *yyoutput;
    int yytype;
    YYSTYPE *yyvaluep;
#endif
{
  /* Pacify ``unused variable'' warnings.  */
  (void) yyvaluep;

  if (yytype < YYNTOKENS)
    {
      YYFPRINTF (yyoutput, "token %s (", yytname[yytype]);
# ifdef YYPRINT
      YYPRINT (yyoutput, yytoknum[yytype], *yyvaluep);
# endif
    }
  else
    YYFPRINTF (yyoutput, "nterm %s (", yytname[yytype]);

  switch (yytype)
    {
      default:
        break;
    }
  YYFPRINTF (yyoutput, ")");
}

#endif /* ! YYDEBUG */
/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

#if defined (__STDC__) || defined (__cplusplus)
static void
yydestruct (int yytype, YYSTYPE *yyvaluep)
#else
static void
yydestruct (yytype, yyvaluep)
    int yytype;
    YYSTYPE *yyvaluep;
#endif
{
  /* Pacify ``unused variable'' warnings.  */
  (void) yyvaluep;

  switch (yytype)
    {

      default:
        break;
    }
}


/* Prevent warnings from -Wmissing-prototypes.  */

#ifdef YYPARSE_PARAM
# if defined (__STDC__) || defined (__cplusplus)
int yyparse (void *YYPARSE_PARAM);
# else
int yyparse ();
# endif
#else /* ! YYPARSE_PARAM */
#if defined (__STDC__) || defined (__cplusplus)
int yyparse ( SelectParser_t * pParser );
#else
int yyparse ();
#endif
#endif /* ! YYPARSE_PARAM */






/*----------.
| yyparse.  |
`----------*/

#ifdef YYPARSE_PARAM
# if defined (__STDC__) || defined (__cplusplus)
int yyparse (void *YYPARSE_PARAM)
# else
int yyparse (YYPARSE_PARAM)
  void *YYPARSE_PARAM;
# endif
#else /* ! YYPARSE_PARAM */
#if defined (__STDC__) || defined (__cplusplus)
int
yyparse ( SelectParser_t * pParser )
#else
int
yyparse (pParser)
     SelectParser_t * pParser ;
#endif
#endif
{
  /* The lookahead symbol.  */
int yychar;

/* The semantic value of the lookahead symbol.  */
YYSTYPE yylval;

/* Number of syntax errors so far.  */
int yynerrs;

  register int yystate;
  register int yyn;
  int yyresult;
  /* Number of tokens to shift before error messages enabled.  */
  int yyerrstatus;
  /* Lookahead token as an internal (translated) token number.  */
  int yytoken = 0;

  /* Three stacks and their tools:
     `yyss': related to states,
     `yyvs': related to semantic values,
     `yyls': related to locations.

     Refer to the stacks thru separate pointers, to allow yyoverflow
     to reallocate them elsewhere.  */

  /* The state stack.  */
  short	yyssa[YYINITDEPTH];
  short *yyss = yyssa;
  register short *yyssp;

  /* The semantic value stack.  */
  YYSTYPE yyvsa[YYINITDEPTH];
  YYSTYPE *yyvs = yyvsa;
  register YYSTYPE *yyvsp;



#define YYPOPSTACK   (yyvsp--, yyssp--)

  YYSIZE_T yystacksize = YYINITDEPTH;

  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;


  /* When reducing, the number of symbols on the RHS of the reduced
     rule.  */
  int yylen;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yystate = 0;
  yyerrstatus = 0;
  yynerrs = 0;
  yychar = YYEMPTY;		/* Cause a token to be read.  */

  /* Initialize stack pointers.
     Waste one element of value and location stack
     so that they stay on the same level as the state stack.
     The wasted elements are never initialized.  */

  yyssp = yyss;
  yyvsp = yyvs;

  goto yysetstate;

/*------------------------------------------------------------.
| yynewstate -- Push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
 yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed. so pushing a state here evens the stacks.
     */
  yyssp++;

 yysetstate:
  *yyssp = yystate;

  if (yyss + yystacksize - 1 <= yyssp)
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYSIZE_T yysize = yyssp - yyss + 1;

#ifdef yyoverflow
      {
	/* Give user a chance to reallocate the stack. Use copies of
	   these so that the &'s don't force the real ones into
	   memory.  */
	YYSTYPE *yyvs1 = yyvs;
	short *yyss1 = yyss;


	/* Each stack pointer address is followed by the size of the
	   data in use in that stack, in bytes.  This used to be a
	   conditional around just the two extra args, but that might
	   be undefined if yyoverflow is a macro.  */
	yyoverflow ("parser stack overflow",
		    &yyss1, yysize * sizeof (*yyssp),
		    &yyvs1, yysize * sizeof (*yyvsp),

		    &yystacksize);

	yyss = yyss1;
	yyvs = yyvs1;
      }
#else /* no yyoverflow */
# ifndef YYSTACK_RELOCATE
      goto yyoverflowlab;
# else
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
	goto yyoverflowlab;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
	yystacksize = YYMAXDEPTH;

      {
	short *yyss1 = yyss;
	union yyalloc *yyptr =
	  (union yyalloc *) YYSTACK_ALLOC (YYSTACK_BYTES (yystacksize));
	if (! yyptr)
	  goto yyoverflowlab;
	YYSTACK_RELOCATE (yyss);
	YYSTACK_RELOCATE (yyvs);

#  undef YYSTACK_RELOCATE
	if (yyss1 != yyssa)
	  YYSTACK_FREE (yyss1);
      }
# endif
#endif /* no yyoverflow */

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;


      YYDPRINTF ((stderr, "Stack size increased to %lu\n",
		  (unsigned long int) yystacksize));

      if (yyss + yystacksize - 1 <= yyssp)
	YYABORT;
    }

  YYDPRINTF ((stderr, "Entering state %d\n", yystate));

  goto yybackup;

/*-----------.
| yybackup.  |
`-----------*/
yybackup:

/* Do appropriate processing given the current state.  */
/* Read a lookahead token if we need one and don't already have one.  */
/* yyresume: */

  /* First try to decide what to do without reference to lookahead token.  */

  yyn = yypact[yystate];
  if (yyn == YYPACT_NINF)
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either YYEMPTY or YYEOF or a valid lookahead symbol.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token: "));
      yychar = YYLEX;
    }

  if (yychar <= YYEOF)
    {
      yychar = yytoken = YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YYDSYMPRINTF ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yyn == 0 || yyn == YYTABLE_NINF)
	goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  if (yyn == YYFINAL)
    YYACCEPT;

  /* Shift the lookahead token.  */
  YYDPRINTF ((stderr, "Shifting token %s, ", yytname[yytoken]));

  /* Discard the token being shifted unless it is eof.  */
  if (yychar != YYEOF)
    yychar = YYEMPTY;

  *++yyvsp = yylval;


  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  yystate = yyn;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- Do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     `$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
        case 7:

    { pParser->AddItem ( &yyvsp[0] ); ;}
    break;

  case 10:

    { pParser->AliasLastItem ( &yyvsp[0] ); ;}
    break;

  case 11:

    { pParser->AliasLastItem ( &yyvsp[0] ); ;}
    break;

  case 12:

    { pParser->AddItem ( &yyvsp[0] ); ;}
    break;

  case 13:

    { pParser->AddItem ( &yyvsp[-1], SPH_AGGR_AVG, &yyvsp[-3], &yyvsp[0] ); ;}
    break;

  case 14:

    { pParser->AddItem ( &yyvsp[-1], SPH_AGGR_MAX, &yyvsp[-3], &yyvsp[0] ); ;}
    break;

  case 15:

    { pParser->AddItem ( &yyvsp[-1], SPH_AGGR_MIN, &yyvsp[-3], &yyvsp[0] ); ;}
    break;

  case 16:

    { pParser->AddItem ( &yyvsp[-1], SPH_AGGR_SUM, &yyvsp[-3], &yyvsp[0] ); ;}
    break;

  case 17:

    { pParser->AddItem ( &yyvsp[-1], SPH_AGGR_CAT, &yyvsp[-3], &yyvsp[0] ); ;}
    break;

  case 18:

    { pParser->AddItem ( "groupby()", &yyvsp[-2], &yyvsp[0] ); ;}
    break;

  case 19:

    { pParser->AddItem ( "count(*)", &yyvsp[-3], &yyvsp[0] ); ;}
    break;

  case 20:

    { pParser->AddItem ( "@distinct", &yyvsp[-4], &yyvsp[0] ); ;}
    break;

  case 22:

    { yyval = yyvsp[-1]; yyval.m_iEnd = yyvsp[-1].m_iEnd; ;}
    break;

  case 23:

    { yyval = yyvsp[-1]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 24:

    { yyval = yyvsp[-1]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 25:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 26:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 27:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 28:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 29:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 30:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 31:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 32:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 33:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 34:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 35:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 36:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 37:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 38:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 39:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 40:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 41:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 42:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 43:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 46:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 47:

    { yyval = yyvsp[-3]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 48:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 51:

    { yyval = yyvsp[-3]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 52:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 53:

    { yyval = yyvsp[-5]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 54:

    { yyval = yyvsp[-5]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 55:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 56:

    { yyval = yyvsp[-4]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 57:

    { yyval = yyvsp[-3]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 66:

    {
			pParser->AddOption ( &yyvsp[-3], &yyvsp[-1] );
		;}
    break;

  case 69:

    { yyval = yyvsp[-1]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 71:

    { yyval = yyvsp[-1]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 96:

    { yyval = yyvsp[-1]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 97:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 98:

    { yyval = yyvsp[-2]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;

  case 99:

    { yyval = yyvsp[-3]; yyval.m_iEnd = yyvsp[0].m_iEnd; ;}
    break;


    }

/* Line 991 of yacc.c.  */


  yyvsp -= yylen;
  yyssp -= yylen;


  YY_STACK_PRINT (yyss, yyssp);

  *++yyvsp = yyval;


  /* Now `shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */

  yyn = yyr1[yyn];

  yystate = yypgoto[yyn - YYNTOKENS] + *yyssp;
  if (0 <= yystate && yystate <= YYLAST && yycheck[yystate] == *yyssp)
    yystate = yytable[yystate];
  else
    yystate = yydefgoto[yyn - YYNTOKENS];

  goto yynewstate;


/*------------------------------------.
| yyerrlab -- here on detecting error |
`------------------------------------*/
yyerrlab:
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
#if YYERROR_VERBOSE
      yyn = yypact[yystate];

      if (YYPACT_NINF < yyn && yyn < YYLAST)
	{
	  YYSIZE_T yysize = 0;
	  int yytype = YYTRANSLATE (yychar);
	  char *yymsg;
	  int yyx, yycount;

	  yycount = 0;
	  /* Start YYX at -YYN if negative to avoid negative indexes in
	     YYCHECK.  */
	  for (yyx = yyn < 0 ? -yyn : 0;
	       yyx < (int) (sizeof (yytname) / sizeof (char *)); yyx++)
	    if (yyx+yyn<int(sizeof(yycheck)/sizeof(yycheck[0])) && yycheck[yyx + yyn] == yyx && yyx != YYTERROR)
	      yysize += yystrlen (yytname[yyx]) + 15, yycount++;
	  yysize += yystrlen ("syntax error, unexpected ") + 1;
	  yysize += yystrlen (yytname[yytype]);
	  yymsg = (char *) YYSTACK_ALLOC (yysize);
	  if (yymsg != 0)
	    {
	      char *yyp = yystpcpy (yymsg, "syntax error, unexpected ");
	      yyp = yystpcpy (yyp, yytname[yytype]);

	      if (yycount < 5)
		{
		  yycount = 0;
		  for (yyx = yyn < 0 ? -yyn : 0;
		       yyx < (int) (sizeof (yytname) / sizeof (char *));
		       yyx++)
		    if (yyx+yyn<int(sizeof(yycheck)/sizeof(yycheck[0])) && yycheck[yyx + yyn] == yyx && yyx != YYTERROR)
		      {
			const char *yyq = ! yycount ? ", expecting " : " or ";
			yyp = yystpcpy (yyp, yyq);
			yyp = yystpcpy (yyp, yytname[yyx]);
			yycount++;
		      }
		}
	      yyerror (pParser, yymsg);
	      YYSTACK_FREE (yymsg);
	    }
	  else
	    yyerror (pParser, "syntax error; also virtual memory exhausted");
	}
      else
#endif /* YYERROR_VERBOSE */
	yyerror (pParser, "syntax error");
    }



  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
	 error, discard it.  */

      /* Return failure if at end of input.  */
      if (yychar == YYEOF)
        {
	  /* Pop the error token.  */
          YYPOPSTACK;
	  /* Pop the rest of the stack.  */
	  while (yyss < yyssp)
	    {
	      YYDSYMPRINTF ("Error: popping", yystos[*yyssp], yyvsp, yylsp);
	      yydestruct (yystos[*yyssp], yyvsp);
	      YYPOPSTACK;
	    }
	  YYABORT;
        }

      YYDSYMPRINTF ("Error: discarding", yytoken, &yylval, &yylloc);
      yydestruct (yytoken, &yylval);
      yychar = YYEMPTY;

    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab2;


/*----------------------------------------------------.
| yyerrlab1 -- error raised explicitly by an action.  |
`----------------------------------------------------*/
//yyerrlab1:

  /* Suppress GCC warning that yyerrlab1 is unused when no action
     invokes YYERROR.  */
#if defined (__GNUC_MINOR__) && 2093 <= (__GNUC__ * 1000 + __GNUC_MINOR__)
//  __attribute__ ((__unused__))
#endif


  goto yyerrlab2;


/*---------------------------------------------------------------.
| yyerrlab2 -- pop states until the error token can be shifted.  |
`---------------------------------------------------------------*/
yyerrlab2:
  yyerrstatus = 3;	/* Each real token shifted decrements this.  */

  for (;;)
    {
      yyn = yypact[yystate];
      if (yyn != YYPACT_NINF)
	{
	  yyn += YYTERROR;
	  if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYTERROR)
	    {
	      yyn = yytable[yyn];
	      if (0 < yyn)
		break;
	    }
	}

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
	YYABORT;

      YYDSYMPRINTF ("Error: popping", yystos[*yyssp], yyvsp, yylsp);
      yydestruct (yystos[yystate], yyvsp);
      yyvsp--;
      yystate = *--yyssp;

      YY_STACK_PRINT (yyss, yyssp);
    }

  if (yyn == YYFINAL)
    YYACCEPT;

  YYDPRINTF ((stderr, "Shifting error token, "));

  *++yyvsp = yylval;


  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturn;

/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturn;

#ifndef yyoverflow
/*----------------------------------------------.
| yyoverflowlab -- parser overflow comes here.  |
`----------------------------------------------*/
yyoverflowlab:
  yyerror (pParser, "parser stack overflow");
  yyresult = 2;
  /* Fall through.  */
#endif

yyreturn:
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
  return yyresult;
}





#if USE_WINDOWS
#pragma warning(pop)
#endif

