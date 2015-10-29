//
// $Id: sphinx.cpp 4310M 2013-11-09 05:22:08Z (local) $
//

//
// Copyright (c) 2001-2013, Andrew Aksyonoff
// Copyright (c) 2008-2013, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "darts-clone.h"
#include "sphinx.h"
#include "sphinxstem.h"
#include "sphinxquery.h"
#include "sphinxutils.h"
#include "sphinxexpr.h"
#include "sphinxfilter.h"
#include "sphinxint.h"
#include "sphinxsearch.h"
#include "sphinxjson.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <time.h>
#include <math.h>
#include <float.h>

#define SPH_UNPACK_BUFFER_SIZE	4096
#define SPH_READ_PROGRESS_CHUNK (8192*1024)
#define SPH_READ_NOPROGRESS_CHUNK (32768*1024)

#if USE_LIBSTEMMER
#include <libstemmer.h>
#endif

#if USE_LIBEXPAT
#define XMLIMPORT
#include "expat.h"

// workaround for expat versions prior to 1.95.7
#ifndef XMLCALL
#define XMLCALL
#endif
#endif

#if USE_LIBICONV
#include "iconv.h"
#endif

#if USE_ZLIB
#include <zlib.h>
#endif

#if USE_ODBC
#include <sql.h>
#endif

#if USE_RE2
#include <string>
#include <re2/re2.h>
#endif

#if USE_RLP
#include "bt_rlp_c.h"
#include <bt_xwchar.h>
#endif

#if USE_WINDOWS
	#include <io.h> // for open()

	// workaround Windows quirks
	#define popen		_popen
	#define pclose		_pclose
	#define snprintf	_snprintf
	#define sphSeek		_lseeki64

	#define stat		_stat64
	#define fstat		_fstat64
	#if _MSC_VER<1400
	#define struct_stat	__stat64
	#else
	#define struct_stat	struct _stat64
	#endif

	#define ICONV_INBUF_CONST	1
#else
	#include <unistd.h>
	#include <sys/time.h>

	#define sphSeek		lseek

	#define struct_stat		struct stat
#endif

#if ( USE_WINDOWS && USE_MYSQL )
	#pragma comment(linker, "/defaultlib:libmysql.lib")
	#pragma message("Automatically linking with libmysql.lib")
#endif

#if ( USE_WINDOWS && USE_PGSQL )
	#pragma comment(linker, "/defaultlib:libpq.lib")
	#pragma message("Automatically linking with libpq.lib")
#endif

#if ( USE_WINDOWS && USE_LIBSTEMMER )
	#pragma comment(linker, "/defaultlib:libstemmer_c.lib")
	#pragma message("Automatically linking with libstemmer_c.lib")
#endif

#if ( USE_WINDOWS && USE_LIBEXPAT )
	#pragma comment(linker, "/defaultlib:libexpat.lib")
	#pragma message("Automatically linking with libexpat.lib")
#endif

#if ( USE_WINDOWS && USE_LIBICONV )
	#pragma comment(linker, "/defaultlib:iconv.lib")
	#pragma message("Automatically linking with iconv.lib")
#endif

#if ( USE_WINDOWS && USE_RE2 )
	#pragma comment(linker, "/defaultlib:re2.lib")
	#pragma message("Automatically linking with re2.lib")
#endif

#if ( USE_WINDOWS && USE_RLP )
	#pragma comment(linker, "/defaultlib:btrlpc.lib")
	#pragma message("Automatically linking with btrlpc.lib")
	#pragma comment(linker, "/defaultlib:btutils.lib")
	#pragma message("Automatically linking with btutils.lib")
#endif

/////////////////////////////////////////////////////////////////////////////

typedef Hitman_c<8> HITMAN;

// logf() is not there sometimes (eg. Solaris 9)
#if !USE_WINDOWS && !HAVE_LOGF
static inline float logf ( float v )
{
	return (float) log ( v );
}
#endif

#if USE_WINDOWS
void localtime_r ( const time_t * clock, struct tm * res )
{
	*res = *localtime ( clock );
}

void gmtime_r ( const time_t * clock, struct tm * res )
{
	*res = *gmtime ( clock );
}
#endif

// forward decl
void sphWarn ( const char * sTemplate, ... ) __attribute__ ( ( format ( printf, 1, 2 ) ) );
static bool sphTruncate ( int iFD );

/////////////////////////////////////////////////////////////////////////////
// GLOBALS
/////////////////////////////////////////////////////////////////////////////

const char *		SPHINX_DEFAULT_SBCS_TABLE	= "0..9, A..Z->a..z, _, a..z, U+A8->U+B8, U+B8, U+C0..U+DF->U+E0..U+FF, U+E0..U+FF";
const char *		SPHINX_DEFAULT_UTF8_TABLE	= "0..9, A..Z->a..z, _, a..z, U+410..U+42F->U+430..U+44F, U+430..U+44F, U+401->U+451, U+451";

const char *		MAGIC_WORD_SENTENCE		= "\3sentence";		// emitted from source on sentence boundary, stored in dictionary
const char *		MAGIC_WORD_PARAGRAPH	= "\3paragraph";	// emitted from source on paragraph boundary, stored in dictionary

bool				g_bJsonStrict				= false;
bool				g_bJsonAutoconvNumbers		= false;
bool				g_bJsonKeynamesToLowercase	= false;

static const int	DEFAULT_READ_BUFFER		= 262144;
static const int	DEFAULT_READ_UNHINTED	= 32768;
static const int	MIN_READ_BUFFER			= 8192;
static const int	MIN_READ_UNHINTED		= 1024;
#define READ_NO_SIZE_HINT 0

static bool			g_bSphQuiet					= false;
static bool			g_bDebugCheck				= false;

static int			g_iReadBuffer			= DEFAULT_READ_BUFFER;
static int			g_iReadUnhinted			= DEFAULT_READ_UNHINTED;

#ifndef SHAREDIR
#define SHAREDIR "."
#endif

CSphString			g_sLemmatizerBase		= SHAREDIR;

#if USE_RLP
CSphString			g_sRLPRoot				= SHAREDIR;
CSphString			g_sRLPEnv				= SHAREDIR"/rlp-environment.xml";
int					g_iRLPMaxBatchSize		= 51200;
int					g_iRLPMaxBatchDocs		= 50;
#endif

// quick hack for indexer crash reporting
// one day, these might turn into a callback or something
int64_t		g_iIndexerCurrentDocID		= 0;
int64_t		g_iIndexerCurrentHits		= 0;
int64_t		g_iIndexerCurrentRangeMin	= 0;
int64_t		g_iIndexerCurrentRangeMax	= 0;
int64_t		g_iIndexerPoolStartDocID	= 0;
int64_t		g_iIndexerPoolStartHit		= 0;


/// global IDF
class CSphGlobalIDF
{
public:
	CSphGlobalIDF ()
		: m_iTotalDocuments ( 0 )
		, m_iTotalWords ( 0 )
	{}

	bool			Touch ( const CSphString & sFilename );
	bool			Preread ( const CSphString & sFilename, CSphString & sError );
	DWORD			GetDocs ( const CSphString & sWord ) const;
	float			GetIDF ( const CSphString & sWord, int64_t iDocsLocal, bool bPlainIDF );

protected:
#pragma pack(push,4)
	struct IDFWord_t
	{
		uint64_t				m_uWordID;
		DWORD					m_iDocs;
	};
#pragma pack(pop)
	STATIC_SIZE_ASSERT			( IDFWord_t, 12 );

	static const int			HASH_BITS = 16;
	int64_t						m_iTotalDocuments;
	int64_t						m_iTotalWords;
	SphOffset_t					m_uMTime;
	CSphSharedBuffer<IDFWord_t>	m_pWords;
	CSphSharedBuffer<int64_t>	m_pHash;
};


/// global idf definitions hash
static SmallStringHash_T <CSphGlobalIDF * >	g_hGlobalIDFs;
static CSphStaticMutex						g_tGlobalIDFLock;

const unsigned char UTF8CharLenTable[256] =
{
        /*
        1-4:    a leading byte of UTF-8 sequence, and the value is the sequence's length
        8:      a content byte(Not a leading byte)
        16:     illegal byte, it never apperas in a UTF-8 sequence
        */
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //0-15
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //16-31
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //32-47
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //48-63

        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //64-79
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //80-95
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //96-111
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, //112-127

        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,

        16, 16, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, //192-207
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, //208-223

        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, //224-239

        4, 4, 4, 4, 4, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16  //240-255
};
//const char * UTF8_CHAR_LEN_TABLE="\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x1\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8\x10\x10\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x2\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x3\x4\x4\x4\x4\x4\x10\x10\x10\x10\x10\x10\x10\x10\x10\x10\x10";
const unsigned char * UTF8_CHAR_LEN_TABLE = UTF8CharLenTable;
void* g_pChineseDictionary = NULL;

/////////////////////////////////////////////////////////////////////////////
// COMPILE-TIME CHECKS
/////////////////////////////////////////////////////////////////////////////

STATIC_SIZE_ASSERT ( SphOffset_t, 8 );

/////////////////////////////////////////////////////////////////////////////

#if !USE_WINDOWS

bool g_bHeadProcess = true;

void sphSetProcessInfo ( bool bHead )
{
	g_bHeadProcess = bHead;
}

#endif // USE_WINDOWS

// whatever to collect IO stats
static bool g_bCollectIOStats = false;
static SphThreadKey_t g_tIOStatsTls;


bool sphInitIOStats ()
{
	if ( !sphThreadKeyCreate ( &g_tIOStatsTls ) )
		return false;

	g_bCollectIOStats = true;
	return true;
}

void sphDoneIOStats ()
{
	sphThreadKeyDelete ( g_tIOStatsTls );
	g_bCollectIOStats = false;
}


CSphIOStats::CSphIOStats ()
	: m_iReadTime ( 0 )
	, m_iReadOps ( 0 )
	, m_iReadBytes ( 0 )
	, m_iWriteTime ( 0 )
	, m_iWriteOps ( 0 )
	, m_iWriteBytes ( 0 )
	, m_pPrev ( NULL )
{}


CSphIOStats::~CSphIOStats ()
{
	Stop();
}


void CSphIOStats::Start()
{
	if ( !g_bCollectIOStats )
		return;

	m_pPrev = (CSphIOStats *)sphThreadGet ( g_tIOStatsTls );
	sphThreadSet ( g_tIOStatsTls, this );
	m_bEnabled = true;
}

void CSphIOStats::Stop()
{
	if ( !g_bCollectIOStats )
		return;

	m_bEnabled = false;
	sphThreadSet ( g_tIOStatsTls, m_pPrev );
}


void CSphIOStats::Add ( const CSphIOStats & b )
{
	m_iReadTime += b.m_iReadTime;
	m_iReadOps += b.m_iReadOps;
	m_iReadBytes += b.m_iReadBytes;
	m_iWriteTime += b.m_iWriteTime;
	m_iWriteOps += b.m_iWriteOps;
	m_iWriteBytes += b.m_iWriteBytes;
}


static CSphIOStats * GetIOStats ()
{
	if ( !g_bCollectIOStats )
		return NULL;

	CSphIOStats * pIOStats = (CSphIOStats *)sphThreadGet ( g_tIOStatsTls );

	if ( !pIOStats || !pIOStats->IsEnabled() )
		return NULL;
	else
		return pIOStats;
}


static size_t sphRead ( int iFD, void * pBuf, size_t iCount )
{
	CSphIOStats * pIOStats = GetIOStats();
	int64_t tmStart = 0;
	if ( pIOStats )
		tmStart = sphMicroTimer();

	size_t uRead = (size_t) ::read ( iFD, pBuf, iCount );

	if ( pIOStats )
	{
		pIOStats->m_iReadTime += sphMicroTimer() - tmStart;
		pIOStats->m_iReadOps++;
		pIOStats->m_iReadBytes += iCount;
	}

	return uRead;
}


static bool GetFileStats ( const char * szFilename, CSphSavedFile & tInfo, CSphString * pError );

/////////////////////////////////////////////////////////////////////////////
// INTERNAL SPHINX CLASSES DECLARATIONS
/////////////////////////////////////////////////////////////////////////////

CSphAutofile::CSphAutofile ()
	: m_iFD ( -1 )
	, m_bTemporary ( false )
	, m_bWouldTemporary ( false )
	, m_pStat ( NULL )
{
}


CSphAutofile::CSphAutofile ( const CSphString & sName, int iMode, CSphString & sError, bool bTemp )
	: m_iFD ( -1 )
	, m_bTemporary ( false )
	, m_bWouldTemporary ( false )
	, m_pStat ( NULL )
{
	Open ( sName, iMode, sError, bTemp );
}


CSphAutofile::~CSphAutofile ()
{
	Close ();
}


int CSphAutofile::Open ( const CSphString & sName, int iMode, CSphString & sError, bool bTemp )
{
	assert ( m_iFD==-1 && m_sFilename.IsEmpty () );
	assert ( !sName.IsEmpty() );

#if USE_WINDOWS
	if ( iMode==SPH_O_READ )
	{
		intptr_t tFD = (intptr_t)CreateFile ( sName.cstr(), GENERIC_READ , FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
		m_iFD = _open_osfhandle ( tFD, 0 );
	} else
		m_iFD = ::open ( sName.cstr(), iMode, 0644 );
#else
	m_iFD = ::open ( sName.cstr(), iMode, 0644 );
#endif
	m_sFilename = sName; // not exactly sure why is this uncoditional. for error reporting later, i suppose

	if ( m_iFD<0 )
		sError.SetSprintf ( "failed to open %s: %s", sName.cstr(), strerror(errno) );
	else
	{
		m_bTemporary = bTemp; // only if we managed to actually open it
		m_bWouldTemporary = true; // if a shit happen - we could delete the file.
	}

	return m_iFD;
}


void CSphAutofile::Close ()
{
	if ( m_iFD>=0 )
	{
		::close ( m_iFD );
		if ( m_bTemporary )
			::unlink ( m_sFilename.cstr() );
	}

	m_iFD = -1;
	m_sFilename = "";
	m_bTemporary = false;
	m_bWouldTemporary = false;
}

void CSphAutofile::SetTemporary()
{
	m_bTemporary = m_bWouldTemporary;
}


const char * CSphAutofile::GetFilename () const
{
	assert ( m_sFilename.cstr() );
	return m_sFilename.cstr();
}


SphOffset_t CSphAutofile::GetSize ( SphOffset_t iMinSize, bool bCheckSizeT, CSphString & sError )
{
	struct_stat st;
	if ( stat ( GetFilename(), &st )<0 )
	{
		sError.SetSprintf ( "failed to stat %s: %s", GetFilename(), strerror(errno) );
		return -1;
	}
	if ( st.st_size<iMinSize )
	{
		sError.SetSprintf ( "failed to load %s: bad size "INT64_FMT" (at least "INT64_FMT" bytes expected)",
			GetFilename(), (int64_t)st.st_size, (int64_t)iMinSize );
		return -1;
	}
	if ( bCheckSizeT )
	{
		size_t sCheck = (size_t)st.st_size;
		if ( st.st_size!=SphOffset_t(sCheck) )
		{
			sError.SetSprintf ( "failed to load %s: bad size "INT64_FMT" (out of size_t; 4 GB limit on 32-bit machine hit?)",
				GetFilename(), (int64_t)st.st_size );
			return -1;
		}
	}
	return st.st_size;
}


SphOffset_t CSphAutofile::GetSize ()
{
	CSphString sTmp;
	return GetSize ( 0, false, sTmp );
}


bool CSphAutofile::Read ( void * pBuf, int64_t iCount, CSphString & sError )
{
	int64_t iToRead = iCount;
	BYTE * pCur = (BYTE *)pBuf;
	while ( iToRead>0 )
	{
		int64_t iToReadOnce = ( m_pStat )
			? Min ( iToRead, SPH_READ_PROGRESS_CHUNK )
			: Min ( iToRead, SPH_READ_NOPROGRESS_CHUNK );
		int64_t iGot = (int64_t) sphRead ( GetFD(), pCur, (size_t)iToReadOnce );
		if ( iGot<=0 )
			break;

		iToRead -= iGot;
		pCur += iGot;

		if ( m_pStat )
		{
			m_pStat->m_iBytes += iGot;
			m_pStat->Show ( false );
		}
	}

	if ( iToRead!=0 )
	{
		sError.SetSprintf ( "read error in %s; "INT64_FMT" of "INT64_FMT" bytes read",
			GetFilename(), iCount-iToRead, iCount );
		return false;
	}
	return true;
}


void CSphAutofile::SetProgressCallback ( CSphIndexProgress * pStat )
{
	m_pStat = pStat;
}


/////////////////////////////////////////////////////////////////////////////

/// generic stateless priority queue
template < typename T, typename COMP > class CSphQueue
{
protected:
	T *		m_pData;
	int		m_iUsed;
	int		m_iSize;

public:
	/// ctor
	explicit CSphQueue ( int iSize )
		: m_iUsed ( 0 )
		, m_iSize ( iSize )
	{
		assert ( iSize>0 );
		m_pData = new T [ iSize ];
		assert ( m_pData );
	}

	/// dtor
	virtual ~CSphQueue ()
	{
		SafeDeleteArray ( m_pData );
	}

	/// add entry to the queue
	virtual bool Push ( const T & tEntry )
	{
		if ( m_iUsed==m_iSize )
		{
			// if it's worse that current min, reject it, else pop off current min
			if ( COMP::IsLess ( tEntry, m_pData[0] ) )
				return true;
			else
				Pop ();
		}

		// do add
		m_pData [ m_iUsed ] = tEntry;
		int iEntry = m_iUsed++;

		// sift up if needed, so that worst (lesser) ones float to the top
		while ( iEntry )
		{
			int iParent = ( iEntry-1 ) >> 1;
			if ( !COMP::IsLess ( m_pData[iEntry], m_pData[iParent] ) )
				break;

			// entry is less than parent, should float to the top
			Swap ( m_pData[iEntry], m_pData[iParent] );
			iEntry = iParent;
		}

		return true;
	}

	/// remove root (ie. top priority) entry
	virtual void Pop ()
	{
		assert ( m_iUsed );
		if ( !(--m_iUsed) ) // empty queue? just return
			return;

		// make the last entry my new root
		m_pData[0] = m_pData[m_iUsed];

		// sift down if needed
		int iEntry = 0;
		for ( ;; )
		{
			// select child
			int iChild = (iEntry<<1) + 1;
			if ( iChild>=m_iUsed )
				break;

			// select smallest child
			if ( iChild+1<m_iUsed )
				if ( COMP::IsLess ( m_pData[iChild+1], m_pData[iChild] ) )
					iChild++;

			// if smallest child is less than entry, do float it to the top
			if ( COMP::IsLess ( m_pData[iChild], m_pData[iEntry] ) )
			{
				Swap ( m_pData[iChild], m_pData[iEntry] );
				iEntry = iChild;
				continue;
			}

			break;
		}
	}

	/// get entries count
	inline int GetLength () const
	{
		return m_iUsed;
	}

	/// get current root
	inline const T & Root () const
	{
		assert ( m_iUsed );
		return m_pData[0];
	}
};

//////////////////////////////////////////////////////////////////////////

/// possible bin states
enum ESphBinState
{
	BIN_ERR_READ	= -2,	///< bin read error
	BIN_ERR_END		= -1,	///< bin end
	BIN_POS			= 0,	///< bin is in "expects pos delta" state
	BIN_DOC			= 1,	///< bin is in "expects doc delta" state
	BIN_WORD		= 2		///< bin is in "expects word delta" state
};


enum ESphBinRead
{
	BIN_READ_OK,			///< bin read ok
	BIN_READ_EOF,			///< bin end
	BIN_READ_ERROR,			///< bin read error
	BIN_PRECACHE_OK,		///< precache ok
	BIN_PRECACHE_ERROR		///< precache failed
};


/// aggregated hit info
struct CSphAggregateHit
{
	SphDocID_t		m_iDocID;		///< document ID
	SphWordID_t		m_iWordID;		///< word ID in current dictionary
	BYTE *			m_sKeyword;		///< word itself (in keywords dictionary case only)
	Hitpos_t		m_iWordPos;		///< word position in current document, or hit count in case of aggregate hit
	FieldMask_t	m_dFieldMask;	///< mask of fields containing this word, 0 for regular hits, non-0 for aggregate hits

	CSphAggregateHit()
		: m_iDocID ( 0 )
		, m_iWordID ( 0 )
		, m_sKeyword ( NULL )
	{}

	int GetAggrCount () const
	{
		assert ( !m_dFieldMask.TestAll ( false ) );
		return m_iWordPos;
	}

	void SetAggrCount ( int iVal )
	{
		m_iWordPos = iVal;
	}
};


static const int MAX_KEYWORD_BYTES = SPH_MAX_WORD_LEN*3+4;


/// bin, block input buffer
struct CSphBin
{
	static const int	MIN_SIZE	= 8192;
	static const int	WARN_SIZE	= 262144;

protected:
	ESphHitless			m_eMode;
	int					m_iSize;

	BYTE *				m_dBuffer;
	BYTE *				m_pCurrent;
	int					m_iLeft;
	int					m_iDone;
	ESphBinState		m_eState;
	bool				m_bWordDict;
	bool				m_bError;	// FIXME? sort of redundant, but states are a mess

	CSphAggregateHit	m_tHit;									///< currently decoded hit
	BYTE				m_sKeyword [ MAX_KEYWORD_BYTES ];	///< currently decoded hit keyword (in keywords dict mode)

#ifndef NDEBUG
	SphWordID_t			m_iLastWordID;
	BYTE				m_sLastKeyword [ MAX_KEYWORD_BYTES ];
#endif

	int					m_iFile;		///< my file
	SphOffset_t *		m_pFilePos;		///< shared current offset in file
	ThrottleState_t *	m_pThrottle;

public:
	SphOffset_t			m_iFilePos;		///< my current offset in file
	int					m_iFileLeft;	///< how much data is still unread from the file

public:
	explicit 			CSphBin ( ESphHitless eMode = SPH_HITLESS_NONE, bool bWordDict = false );
						~CSphBin ();

	static int			CalcBinSize ( int iMemoryLimit, int iBlocks, const char * sPhase, bool bWarn = true );
	void				Init ( int iFD, SphOffset_t * pSharedOffset, const int iBinSize );

	SphWordID_t			ReadVLB ();
	int					ReadByte ();
	ESphBinRead			ReadBytes ( void * pDest, int iBytes );
	int					ReadHit ( CSphAggregateHit * pHit, int iRowitems, CSphRowitem * pRowitems );

	DWORD				UnzipInt ();
	SphOffset_t			UnzipOffset ();

	bool				IsEOF () const;
	bool				IsDone () const;
	bool				IsError () const { return m_bError; }
	ESphBinRead			Precache ();
	void				SetThrottle ( ThrottleState_t * pState ) { m_pThrottle = pState; }
};

/////////////////////////////////////////////////////////////////////////////

class CSphIndex_VLN;

/// everything required to setup search term
class DiskIndexQwordSetup_c : public ISphQwordSetup
{
public:
	const CSphAutofile &	m_tDoclist;
	const CSphAutofile &	m_tHitlist;
	const CSphAutofile &	m_tWordlist;
	bool					m_bSetupReaders;
	const BYTE *			m_pSkips;
	BYTE *					m_pDictBuf;
	CSphQueryProfile *		m_pProfile;

public:
	DiskIndexQwordSetup_c ( const CSphAutofile & tDoclist, const CSphAutofile & tHitlist, const CSphAutofile & tWordlist, int iDictBufSize, const BYTE * pSkips, CSphQueryProfile * pProfile )
		: m_tDoclist ( tDoclist )
		, m_tHitlist ( tHitlist )
		, m_tWordlist ( tWordlist )
		, m_bSetupReaders ( false )
		, m_pSkips ( pSkips )
		, m_pDictBuf ( NULL )
		, m_pProfile ( pProfile )
	{
		if ( iDictBufSize>0 )
			m_pDictBuf = new BYTE [iDictBufSize];
	}

	virtual ~DiskIndexQwordSetup_c()
	{
		SafeDeleteArray ( m_pDictBuf );
	}

	virtual ISphQword *					QwordSpawn ( const XQKeyword_t & tWord ) const;
	virtual bool						QwordSetup ( ISphQword * ) const;

protected:
	bool								Setup ( ISphQword * ) const;
};


/// query word from the searcher's point of view
class DiskIndexQwordTraits_c : public ISphQword
{
	static const int	MINIBUFFER_LEN = 1024;

public:
	/// tricky bit
	/// m_uHitPosition is always a current position in the .spp file
	/// base ISphQword::m_iHitlistPos carries the inlined hit data when m_iDocs==1
	/// but this one is always a real position, used for delta coding
	SphOffset_t		m_uHitPosition;
	Hitpos_t		m_uInlinedHit;
	DWORD			m_uHitState;

	bool			m_bDupe;		///< whether the word occurs only once in current query

	CSphMatch		m_tDoc;			///< current match (partial)
	Hitpos_t		m_iHitPos;		///< current hit postition, from hitlist

	BYTE			m_dDoclistBuf [ MINIBUFFER_LEN ];
	BYTE			m_dHitlistBuf [ MINIBUFFER_LEN ];
	CSphReader		m_rdDoclist;	///< my doclist reader
	CSphReader		m_rdHitlist;	///< my hitlist reader

	SphDocID_t		m_iMinID;		///< min ID to fixup
	int				m_iInlineAttrs;	///< inline attributes count

	const CSphRowitem *	m_pInlineFixup;	///< inline attributes fixup (POINTER TO EXTERNAL DATA, NOT MANAGED BY THIS CLASS!)

#ifndef NDEBUG
	bool			m_bHitlistOver;
#endif

public:
	explicit DiskIndexQwordTraits_c ( bool bUseMini, bool bExcluded )
		: m_uHitPosition ( 0 )
		, m_uHitState ( 0 )
		, m_bDupe ( false )
		, m_iHitPos ()
		, m_rdDoclist ( bUseMini ? m_dDoclistBuf : NULL, bUseMini ? MINIBUFFER_LEN : 0 )
		, m_rdHitlist ( bUseMini ? m_dHitlistBuf : NULL, bUseMini ? MINIBUFFER_LEN : 0 )
		, m_iMinID ( 0 )
		, m_iInlineAttrs ( 0 )
		, m_pInlineFixup ( NULL )
#ifndef NDEBUG
		, m_bHitlistOver ( true )
#endif
	{
		m_iHitPos = EMPTY_HIT;
		m_bExcluded = bExcluded;
	}
};


bool operator < ( const SkiplistEntry_t & a, SphDocID_t b )		{ return a.m_iBaseDocid<b; }
bool operator == ( const SkiplistEntry_t & a, SphDocID_t b )	{ return a.m_iBaseDocid==b; }
bool operator < ( SphDocID_t a, const SkiplistEntry_t & b )		{ return a<b.m_iBaseDocid; }


/// query word from the searcher's point of view
template < bool INLINE_HITS, bool INLINE_DOCINFO, bool DISABLE_HITLIST_SEEK, bool DO_DEBUG_CHECK >
class DiskIndexQword_c : public DiskIndexQwordTraits_c
{
public:
	explicit DiskIndexQword_c ( bool bUseMinibuffer, bool bExcluded )
		: DiskIndexQwordTraits_c ( bUseMinibuffer, bExcluded )
	{}

	virtual void Reset ()
	{
		m_uHitPosition = 0;
		m_uHitState = 0;
		m_rdDoclist.Reset ();
		m_rdDoclist.Reset ();
		ISphQword::Reset();
		m_iHitPos = EMPTY_HIT;
		m_iInlineAttrs = 0;
	}

	void GetHitlistEntry ()
	{
		assert ( !m_bHitlistOver );
		DWORD iDelta = m_rdHitlist.UnzipInt ();
		if ( iDelta )
		{
			m_iHitPos += iDelta;
		} else
		{
			m_iHitPos = EMPTY_HIT;
#ifndef NDEBUG
			m_bHitlistOver = true;
#endif
		}
	}

	virtual void HintDocid ( SphDocID_t uMinID )
	{
		// tricky bit
		// FindSpan() will match a block where BaseDocid is >= RefValue
		// meaning that the subsequent ids decoded will be strictly > RefValue
		// meaning that if previous (!) blocks end with uMinID exactly,
		// and we use uMinID itself as RefValue, that document gets lost!
		// OPTIMIZE? keep last matched block index maybe?
		int iBlock = FindSpan ( m_dSkiplist, uMinID - m_iMinID - 1 );
		if ( iBlock<0 )
			return;
		const SkiplistEntry_t & t = m_dSkiplist [ iBlock ];
		if ( t.m_iOffset<=m_rdDoclist.GetPos() )
			return;
		m_rdDoclist.SeekTo ( t.m_iOffset, -1 );
		m_tDoc.m_iDocID = t.m_iBaseDocid + m_iMinID;
		m_uHitPosition = m_iHitlistPos = t.m_iBaseHitlistPos;
	}

	virtual const CSphMatch & GetNextDoc ( DWORD * pDocinfo )
	{
		SphDocID_t iDelta = m_rdDoclist.UnzipDocid();
		if ( iDelta )
		{
			m_bAllFieldsKnown = false;
			m_tDoc.m_iDocID += iDelta;
			if_const ( INLINE_DOCINFO )
			{
				assert ( pDocinfo );
				for ( int i=0; i<m_iInlineAttrs; i++ )
					pDocinfo[i] = m_rdDoclist.UnzipInt() + m_pInlineFixup[i];
			}

			if_const ( INLINE_HITS )
			{
				m_uMatchHits = m_rdDoclist.UnzipInt();
				const DWORD uFirst = m_rdDoclist.UnzipInt();
				if ( m_uMatchHits==1 && m_bHasHitlist )
				{
					DWORD uField = m_rdDoclist.UnzipInt(); // field and end marker
					m_iHitlistPos = uFirst | ( uField << 23 ) | ( U64C(1)<<63 );
					m_dQwordFields.UnsetAll();
					if_const ( DO_DEBUG_CHECK )
					{
						if ( ( uField>>1 )>=SPH_MAX_FIELDS )
							uField = ( (DWORD)SPH_MAX_FIELDS-1 )<<1;
					}
					m_dQwordFields.Set ( uField >> 1 );
					m_bAllFieldsKnown = true;
				} else
				{
					m_dQwordFields.Assign32 ( uFirst );
					m_uHitPosition += m_rdDoclist.UnzipOffset();
					m_iHitlistPos = m_uHitPosition;
				}
			} else
			{
				SphOffset_t iDeltaPos = m_rdDoclist.UnzipOffset();
				assert ( iDeltaPos>=0 );

				m_iHitlistPos += iDeltaPos;

				m_dQwordFields.Assign32 ( m_rdDoclist.UnzipInt() );
				m_uMatchHits = m_rdDoclist.UnzipInt();
			}
		} else
		{
			m_tDoc.m_iDocID = 0;
		}
		return m_tDoc;
	}

	virtual void SeekHitlist ( SphOffset_t uOff )
	{
		if ( uOff >> 63 )
		{
			m_uHitState = 1;
			m_uInlinedHit = (DWORD)uOff; // truncate high dword
		} else
		{
			m_uHitState = 0;
			m_iHitPos = EMPTY_HIT;
			if_const ( DISABLE_HITLIST_SEEK )
				assert ( m_rdHitlist.GetPos()==uOff ); // make sure we're where caller thinks we are.
			else
				m_rdHitlist.SeekTo ( uOff, READ_NO_SIZE_HINT );
		}
#ifndef NDEBUG
		m_bHitlistOver = false;
#endif
	}

	virtual Hitpos_t GetNextHit ()
	{
		assert ( m_bHasHitlist );
		switch ( m_uHitState )
		{
			case 0: // read hit from hitlist
				GetHitlistEntry ();
				return m_iHitPos;

			case 1: // return inlined hit
				m_uHitState = 2;
				return m_uInlinedHit;

			case 2: // return end-of-hitlist marker after inlined hit
				#ifndef NDEBUG
				m_bHitlistOver = true;
				#endif
				m_uHitState = 0;
				return EMPTY_HIT;
		}
		sphDie ( "INTERNAL ERROR: impossible hit emitter state" );
		return EMPTY_HIT;
	}
};

//////////////////////////////////////////////////////////////////////////////

#define WITH_QWORD(INDEX, NO_SEEK, NAME, ACTION)													\
{																									\
	CSphIndex_VLN * INDEX##pIndex = (CSphIndex_VLN *)INDEX;												\
	DWORD INDEX##uInlineHits = INDEX##pIndex->m_tSettings.m_eHitFormat==SPH_HIT_FORMAT_INLINE;					\
	DWORD INDEX##uInlineDocinfo = INDEX##pIndex->m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE;						\
																									\
	switch ( ( INDEX##uInlineHits<<1 ) | INDEX##uInlineDocinfo )													\
	{																								\
		case 0: { typedef DiskIndexQword_c < false, false, NO_SEEK, false > NAME; ACTION; break; }			\
		case 1: { typedef DiskIndexQword_c < false, true, NO_SEEK, false > NAME; ACTION; break; }			\
		case 2: { typedef DiskIndexQword_c < true, false, NO_SEEK, false > NAME; ACTION; break; }			\
		case 3: { typedef DiskIndexQword_c < true, true, NO_SEEK, false > NAME; ACTION; break; }			\
		default:																					\
			sphDie ( "INTERNAL ERROR: impossible qword settings" );									\
	}																								\
}

/////////////////////////////////////////////////////////////////////////////

struct CSphWordlistCheckpoint
{
	union
	{
		SphWordID_t		m_iWordID;
		const char *	m_sWord;
	};
	SphOffset_t			m_iWordlistOffset;
};

// pre-v11 wordlist checkpoint
struct CSphWordlistCheckpoint_v10
{
	SphWordID_t			m_iWordID;
	DWORD				m_iWordlistOffset;
};

/////////////////////////////////////////////////////////////////////////////

/// ordinals accumulation and sorting
struct Ordinal_t
{
	SphDocID_t	m_uDocID;	///< doc id
	CSphString	m_sValue;	///< string value
};


struct OrdinalEntry_t : public Ordinal_t
{
	int	m_iTag;
};


struct OrdinalId_t
{
	SphDocID_t	m_uDocID;
	DWORD		m_uId;
};


struct OrdinalIdEntry_t : public OrdinalId_t
{
	int	m_iTag;
};

void Swap ( Ordinal_t & a, Ordinal_t & b )
{
	Swap ( a.m_uDocID, b.m_uDocID );
	Swap ( a.m_sValue, b.m_sValue );
}

void Swap ( OrdinalEntry_t & a, OrdinalEntry_t & b )
{
	Swap ( a.m_uDocID, b.m_uDocID );
	Swap ( a.m_sValue, b.m_sValue );
	Swap ( a.m_iTag, b.m_iTag );
}

//////////////////////////////////////////////////////////////////////////

static void ReadFileInfo ( CSphReader & tReader, const char * szFilename, CSphSavedFile & tFile, CSphString * sWarning )
{
	tFile.m_uSize = tReader.GetOffset ();
	tFile.m_uCTime = tReader.GetOffset ();
	tFile.m_uMTime = tReader.GetOffset ();
	tFile.m_uCRC32 = tReader.GetDword ();
	tFile.m_sFilename = szFilename;

	if ( szFilename && *szFilename && sWarning )
	{
		struct_stat tFileInfo;
		if ( stat ( szFilename, &tFileInfo ) < 0 )
			sWarning->SetSprintf ( "failed to stat %s: %s", szFilename, strerror(errno) );
		else
		{
			DWORD uMyCRC32 = 0;
			if ( !sphCalcFileCRC32 ( szFilename, uMyCRC32 ) )
				sWarning->SetSprintf ( "failed to calculate CRC32 for %s", szFilename );
			else
				if ( uMyCRC32!=tFile.m_uCRC32 || tFileInfo.st_size!=tFile.m_uSize
					|| tFileInfo.st_ctime!=tFile.m_uCTime || tFileInfo.st_mtime!=tFile.m_uMTime )
						sWarning->SetSprintf ( "'%s' differs from the original", szFilename );
		}
	}
}


static void WriteFileInfo ( CSphWriter & tWriter, const CSphSavedFile & tInfo )
{
	tWriter.PutOffset ( tInfo.m_uSize );
	tWriter.PutOffset ( tInfo.m_uCTime );
	tWriter.PutOffset ( tInfo.m_uMTime );
	tWriter.PutDword ( tInfo.m_uCRC32 );
}


/// dict=keywords block reader
class KeywordsBlockReader_c : public CSphDictEntry
{
private:
	const BYTE *	m_pBuf;
	BYTE			m_sWord [ MAX_KEYWORD_BYTES ];
	int				m_iLen;
	BYTE			m_uHint;
	bool			m_bHaveSkips;

public:
	explicit		KeywordsBlockReader_c ( const BYTE * pBuf, bool bHaveSkiplists );
	bool			UnpackWord();

	const char *	GetWord() const			{ return (const char*)m_sWord; }
	int				GetWordLen() const		{ return m_iLen; }
};


// dictionary header
struct DictHeader_t
{
	int				m_iDictCheckpoints;			///< how many dict checkpoints (keyword blocks) are there
	SphOffset_t		m_iDictCheckpointsOffset;	///< dict checkpoints file position

	int				m_iInfixCodepointBytes;		///< max bytes per infix codepoint (0 means no infixes)
	int				m_iInfixBlocksOffset;		///< infix blocks file position (32bit as keywords dictionary is pretty small)
	int				m_iInfixBlocksWordsSize;	///< infix checkpoints size

	DictHeader_t()
		: m_iDictCheckpoints ( 0 )
		, m_iDictCheckpointsOffset ( 0 )
		, m_iInfixCodepointBytes ( 0 )
		, m_iInfixBlocksOffset ( 0 )
		, m_iInfixBlocksWordsSize ( 0 )
	{}
};


// !COMMIT eliminate this, move it to proper dict impls
class CWordlist : public ISphWordlist, public DictHeader_t
{
public:
	CSphFixedVector<CSphWordlistCheckpoint>	m_dCheckpoints;		///< checkpoint offsets
	CSphVector<InfixBlock_t>				m_dInfixBlocks;

	CSphAutofile						m_tFile;				///< file
	int64_t								m_iSize;				///< file size
	CSphSharedBuffer<BYTE>				m_pBuf;					///< my cache
	int									m_iMaxChunk;			///< max size of entry between checkpoints
	SphOffset_t							m_iWordsEnd;			///< end of wordlist
	bool								m_bHaveSkips;			///< whether there are skiplists

	BYTE *								m_pWords;				///< arena for checkpoint's words
	BYTE *								m_pInfixBlocksWords;	///< arena for infix checkpoint's words

public:
	explicit							CWordlist ();
										~CWordlist ();
	void								Reset ();

	bool								ReadCP ( CSphAutofile & tFile, DWORD uVersion, bool bWordDict, CSphString & sError );

	const CSphWordlistCheckpoint *		FindCheckpoint ( const char * sWord, int iWordLen, SphWordID_t iWordID, bool bStarMode ) const;
	bool								GetWord ( const BYTE * pBuf, SphWordID_t iWordID, CSphDictEntry & tWord ) const;

	const BYTE *						AcquireDict ( const CSphWordlistCheckpoint * pCheckpoint, int iFD, BYTE * pDictBuf ) const;
	virtual void						GetPrefixedWords ( const char * sPrefix, int iPrefixLen, const char * sWildcard, CSphVector<CSphNamedInt> & dExpanded, BYTE * pDictBuf, int iFD ) const;
	virtual void						GetInfixedWords ( const char * sInfix, int iInfix, const char * sWildcard, CSphVector<CSphNamedInt> & dPrefixedWords ) const;

private:
	bool								m_bWordDict;
};


class CSphHitBuilder;


struct BuildHeader_t : public CSphSourceStats, public DictHeader_t
{
	explicit BuildHeader_t ( const CSphSourceStats & tStat )
		: m_sHeaderExtension ( NULL )
		, m_pThrottle ( NULL )
		, m_pMinRow ( NULL )
		, m_iMinDocid ( 0 )
		, m_iKillListSize ( 0 )
		, m_uMinMaxIndex ( 0 )
		, m_iTotalDups ( 0 )
	{
		m_iTotalDocuments = tStat.m_iTotalDocuments;
		m_iTotalBytes = tStat.m_iTotalBytes;
	}

	const char *		m_sHeaderExtension;
	ThrottleState_t *	m_pThrottle;
	const CSphRowitem *	m_pMinRow;
	SphDocID_t			m_iMinDocid;
	DWORD				m_iKillListSize;
	int64_t				m_uMinMaxIndex;
	int					m_iTotalDups;
};


DWORD ReadVersion ( const char * sPath, CSphString & sError )
{
	BYTE dBuffer[8];
	char sHeaderName [ SPH_MAX_FILENAME_LEN ];

	snprintf ( sHeaderName, sizeof(sHeaderName), "%s%s", sPath, ".new.sph" );

	CSphAutoreader rdHeader ( dBuffer, sizeof(dBuffer) );
	if ( !rdHeader.Open ( sHeaderName, sError ) )
		return 0;

	DWORD uHeader = rdHeader.GetDword();
	if ( uHeader!=INDEX_MAGIC_HEADER )
	{
		sError.SetSprintf ( "%s is invalid header file (too old index version?)", sHeaderName );
		return 0;
	}

	DWORD uVersion = rdHeader.GetDword();
	if ( uVersion==0 || uVersion>INDEX_FORMAT_VERSION )
	{
		sError.SetSprintf ( "%s is v.%d, binary is v.%d", sHeaderName, uVersion, INDEX_FORMAT_VERSION );
		return 0;
	}

	return uVersion;
}


static const char * g_dNewExts17[] = { ".new.sph", ".new.spa", ".new.spi", ".new.spd", ".new.spp", ".new.spm", ".new.spk", ".new.sps" };
static const char * g_dOldExts17[] = { ".old.sph", ".old.spa", ".old.spi", ".old.spd", ".old.spp", ".old.spm", ".old.spk", ".old.sps", ".old.mvp" };
static const char * g_dCurExts17[] = { ".sph", ".spa", ".spi", ".spd", ".spp", ".spm", ".spk", ".sps", ".mvp" };
static const char * g_dLocExts17[] = { ".sph", ".spa", ".spi", ".spd", ".spp", ".spm", ".spk", ".sps", ".spl" };

static const char * g_dNewExts31[] = { ".new.sph", ".new.spa", ".new.spi", ".new.spd", ".new.spp", ".new.spm", ".new.spk", ".new.sps", ".new.spe" };
static const char * g_dOldExts31[] = { ".old.sph", ".old.spa", ".old.spi", ".old.spd", ".old.spp", ".old.spm", ".old.spk", ".old.sps", ".old.spe", ".old.mvp" };
static const char * g_dCurExts31[] = { ".sph", ".spa", ".spi", ".spd", ".spp", ".spm", ".spk", ".sps", ".spe", ".mvp" };
static const char * g_dLocExts31[] = { ".sph", ".spa", ".spi", ".spd", ".spp", ".spm", ".spk", ".sps", ".spe", ".spl" };

static const char ** g_pppAllExts[] = { g_dCurExts31, g_dNewExts31, g_dOldExts31, g_dLocExts31 };


const char ** sphGetExts ( ESphExtType eType, DWORD uVersion )
{
	if ( uVersion<31 )
	{
		switch ( eType )
		{
		case SPH_EXT_TYPE_NEW: return g_dNewExts17;
		case SPH_EXT_TYPE_OLD: return g_dOldExts17;
		case SPH_EXT_TYPE_CUR: return g_dCurExts17;
		case SPH_EXT_TYPE_LOC: return g_dLocExts17;
		}

	} else
	{
		switch ( eType )
		{
		case SPH_EXT_TYPE_NEW: return g_dNewExts31;
		case SPH_EXT_TYPE_OLD: return g_dOldExts31;
		case SPH_EXT_TYPE_CUR: return g_dCurExts31;
		case SPH_EXT_TYPE_LOC: return g_dLocExts31;
		}
	}

	assert ( 0 && "Unknown extension type" );
	return NULL;
}

int sphGetExtCount ( DWORD uVersion )
{
	if ( uVersion<31 )
		return 8;
	else
		return 9;
}

const char * sphGetExt ( ESphExtType eType, ESphExt eExt )
{
	if ( eExt==SPH_EXT_MVP )
	{
		assert ( eType==SPH_EXT_TYPE_CUR || eType==SPH_EXT_TYPE_OLD );
		return g_pppAllExts[eType][eExt];
	}

	assert ( eExt>=0 && eExt<=(int)sizeof(g_pppAllExts[0])/(int)sizeof(g_pppAllExts[0][0]));

	return g_pppAllExts[eType][eExt];
}

/// this pseudo-index used to store and manage the tokenizer
/// without any footprint in real files
//////////////////////////////////////////////////////////////////////////
static CSphSourceStats g_tTmpDummyStat;
class CSphTokenizerIndex : public CSphIndex
{
public:
	CSphTokenizerIndex () : CSphIndex ( NULL, NULL ) {}
	virtual SphAttr_t *			GetKillList () const { return NULL; }
	virtual int					GetKillListSize () const { return 0 ; }
	virtual bool				HasDocid ( SphDocID_t ) const { return false; }
	virtual int					Build ( const CSphVector<CSphSource*> & , int , int ) { return 0; }
	virtual bool				Merge ( CSphIndex * , const CSphVector<CSphFilterSettings> & , bool ) {return false; }
	virtual bool				Prealloc ( bool , bool , CSphString & ) { return false; }
	virtual void				Dealloc () {}
	virtual bool				Preread () { return false; }
	virtual void				SetBase ( const char * ) {}
	virtual bool				Rename ( const char * ) { return false; }
	virtual bool				Lock () { return false; }
	virtual void				Unlock () {}
	virtual bool				Mlock () { return false; }
	virtual void				PostSetup() {}
	virtual bool				EarlyReject ( CSphQueryContext * , CSphMatch & ) const { return false; }
	virtual const CSphSourceStats &	GetStats () const { return g_tTmpDummyStat; }
	virtual CSphIndexStatus			GetStatus () const { CSphIndexStatus tRes; tRes.m_iDiskUse = 0; tRes.m_iRamUse = 0; return tRes; }
	virtual bool				MultiQuery ( const CSphQuery * , CSphQueryResult * , int , ISphMatchSorter ** , const CSphMultiQueryArgs & ) const { return false; }
	virtual bool				MultiQueryEx ( int , const CSphQuery * , CSphQueryResult ** , ISphMatchSorter ** , const CSphMultiQueryArgs & ) const { return false; }
	virtual bool				GetKeywords ( CSphVector <CSphKeywordInfo> & , const char * , bool , CSphString * ) const;
	virtual bool				FillKeywords ( CSphVector <CSphKeywordInfo> & ) const { return true; }
	virtual int					UpdateAttributes ( const CSphAttrUpdate & , int , CSphString & , CSphString & ) { return -1; }
	virtual bool				SaveAttributes ( CSphString & ) const { return false; }
	virtual DWORD				GetAttributeStatus () const { return 0; }
	virtual bool				CreateFilesWithAttr ( int , const CSphString & , ESphAttr , CSphString & ) { return true; }
	virtual bool				AddAttribute ( const CSphString &, ESphAttr, CSphString & ) { return true; }
	virtual void				DebugDumpHeader ( FILE *, const char *, bool ) {}
	virtual void				DebugDumpDocids ( FILE * ) {}
	virtual void				DebugDumpHitlist ( FILE * , const char * , bool ) {}
	virtual int					DebugCheck ( FILE * ) { return 0; } // NOLINT
	virtual void				DebugDumpDict ( FILE * ) {}
	virtual	void				SetProgressCallback ( CSphIndexProgress::IndexingProgress_fn ) {}
};

bool CSphTokenizerIndex::GetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords,
								const char * szQuery, bool, CSphString * ) const
{
	// short-cut if no query or keywords to fill
	if ( !szQuery || !szQuery[0] )
		return true;

	CSphScopedPtr<ISphTokenizer> pTokenizer ( m_pTokenizer->Clone ( SPH_CLONE_INDEX ) ); // avoid race
	pTokenizer->EnableTokenizedMultiformTracking ();

	// need to support '*' and '=' but not the other specials
	// so m_pQueryTokenizer does not work for us, gotta clone and setup one manually
	if ( IsStarDict() )
		pTokenizer->AddPlainChar ( '*' );
	if ( m_tSettings.m_bIndexExactWords )
		pTokenizer->AddPlainChar ( '=' );


	CSphScopedPtr<CSphDict> tDictCloned ( NULL );
	CSphDict * pDictBase = m_pDict;
	if ( pDictBase->HasState() )
		tDictCloned = pDictBase = pDictBase->Clone();

	CSphDict * pDict = pDictBase;
	if ( IsStarDict() )
		pDict = new CSphDictStar ( pDictBase );

	if ( m_tSettings.m_bIndexExactWords )
		pDict = new CSphDictExact ( pDict );

	dKeywords.Resize ( 0 );
	CSphString sTokenized;
	BYTE * sWord;

	CSphString sQbuf ( szQuery );
	pTokenizer->SetBuffer ( (BYTE*)sQbuf.cstr(), strlen(szQuery) );

	while ( ( sWord = pTokenizer->GetToken() )!=NULL )
	{
		BYTE * sMultiform = pTokenizer->GetTokenizedMultiform();
		if ( sMultiform )
			sTokenized = (const char*)sMultiform;
		else
			sTokenized = (const char*)sWord;

		// result unused, however the stemmers applied
		pDict->GetWordID ( sWord );

		CSphKeywordInfo & tInfo = dKeywords.Add();
		Swap ( tInfo.m_sTokenized, sTokenized );
		tInfo.m_sNormalized = (const char*)sWord;
		tInfo.m_iDocs = 0;
		tInfo.m_iHits = 0;

		if ( tInfo.m_sNormalized.cstr()[0]==MAGIC_WORD_HEAD_NONSTEMMED )
			*(char *)tInfo.m_sNormalized.cstr() = '=';
	}
	return true;
}


CSphIndex *			sphCreateIndexTemplate ( )
{
	return new CSphTokenizerIndex();
}

/// this is my actual VLN-compressed phrase index implementation
class CSphIndex_VLN : public CSphIndex
{
	friend class DiskIndexQwordSetup_c;
	friend class CSphMerger;
	friend class AttrIndexBuilder_t<SphDocID_t>;
	friend struct SphFinalMatchCalc_t;

public:
	explicit					CSphIndex_VLN ( const char* sIndexName, const char * sFilename );
								~CSphIndex_VLN ();

	virtual int					Build ( const CSphVector<CSphSource*> & dSources, int iMemoryLimit, int iWriteBuffer );
	virtual	void				SetProgressCallback ( CSphIndexProgress::IndexingProgress_fn pfnProgress ) { m_tProgress.m_fnProgress = pfnProgress; }

	virtual bool				LoadHeader ( const char * sHeaderName, bool bStripPath, CSphEmbeddedFiles & tEmbeddedFiles, CSphString & sWarning );
	virtual bool				WriteHeader ( const BuildHeader_t & tBuildHeader, CSphWriter & fdInfo ) const;

	virtual void				DebugDumpHeader ( FILE * fp, const char * sHeaderName, bool bConfig );
	virtual void				DebugDumpDocids ( FILE * fp );
	virtual void				DebugDumpHitlist ( FILE * fp, const char * sKeyword, bool bID );
	virtual void				DebugDumpDict ( FILE * fp );
	virtual int					DebugCheck ( FILE * fp );
	template <class Qword> void	DumpHitlist ( FILE * fp, const char * sKeyword, bool bID );

	virtual bool				Prealloc ( bool bMlock, bool bStripPath, CSphString & sWarning );
	virtual bool				Mlock ();
	virtual void				Dealloc ();
	virtual void				SetEnableOndiskAttributes ( bool bPool );

	virtual bool				Preread ();
	template<typename T> bool	PrereadSharedBuffer ( CSphSharedBuffer<T> & pBuffer, const char * sExt, int64_t iExpected=0, int64_t iOffset=0 );

	virtual void				SetBase ( const char * sNewBase );
	virtual bool				Rename ( const char * sNewBase );

	virtual bool				Lock ();
	virtual void				Unlock ();
	virtual void				PostSetup() {}

	virtual bool				MultiQuery ( const CSphQuery * pQuery, CSphQueryResult * pResult, int iSorters, ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const;
	virtual bool				MultiQueryEx ( int iQueries, const CSphQuery * pQueries, CSphQueryResult ** ppResults, ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const;
	virtual bool				GetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords, const char * szQuery, bool bGetStats, CSphString * pError ) const;
	template <class Qword> bool	DoGetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords, const char * szQuery, bool bGetStats, bool bFillOnly, CSphString * pError ) const;
	virtual bool 				FillKeywords ( CSphVector <CSphKeywordInfo> & dKeywords ) const;

	virtual bool				Merge ( CSphIndex * pSource, const CSphVector<CSphFilterSettings> & dFilters, bool bMergeKillLists );

	template <class QWORDDST, class QWORDSRC>
	static bool					MergeWords ( const CSphIndex_VLN * pDstIndex, const CSphIndex_VLN * pSrcIndex, const ISphFilter * pFilter, SphDocID_t iMinID, CSphHitBuilder * pHitBuilder, CSphString & sError, CSphSourceStats & tStat, CSphIndexProgress & tProgress, ThrottleState_t * pThrottle, volatile bool * pForceTerminate );
	static bool					DoMerge ( const CSphIndex_VLN * pDstIndex, const CSphIndex_VLN * pSrcIndex, bool bMergeKillLists, ISphFilter * pFilter, CSphString & sError, CSphIndexProgress & tProgress, ThrottleState_t * pThrottle, volatile bool * pForceTerminate );

	virtual int					UpdateAttributes ( const CSphAttrUpdate & tUpd, int iIndex, CSphString & sError, CSphString & sWarning );
	virtual bool				SaveAttributes ( CSphString & sError ) const;
	virtual DWORD				GetAttributeStatus () const;

	virtual bool				CreateFilesWithAttr ( int iPos, const CSphString & sAttrName, ESphAttr eAttrType, CSphString & sError );
	virtual bool				AddAttribute ( const CSphString & sAttrName, ESphAttr eAttrType, CSphString & sError );

	bool						EarlyReject ( CSphQueryContext * pCtx, CSphMatch & tMatch ) const;

	virtual void				SetKeepAttrs ( bool bKeepAttrs ) { m_bKeepAttrs = bKeepAttrs; }

	virtual SphAttr_t *			GetKillList () const;
	virtual int					GetKillListSize () const { return m_iKillListSize; }
	virtual bool				HasDocid ( SphDocID_t uDocid ) const;

	virtual const CSphSourceStats &		GetStats () const { return m_tStats; }
	virtual int64_t *					GetFieldLens() const { return m_tSettings.m_bIndexFieldLens ? m_dFieldLens.Begin() : NULL; }
	virtual CSphIndexStatus				GetStatus () const;
	virtual bool 				BuildDocList ( SphAttr_t ** ppDocList, int64_t * pCount, CSphString * pError ) const;
	virtual bool				ReplaceKillist ( const SphAttr_t * pKillist, int iCount );

private:

	static const int			MIN_WRITE_BUFFER		= 262144;	///< min write buffer size
	static const int			DEFAULT_WRITE_BUFFER	= 1048576;	///< default write buffer size

private:
	// common stuff
	int							m_iLockFD;
	CSphSourceStats				m_tStats;			///< my stats
	int							m_iTotalDups;
	CSphFixedVector<CSphRowitem>	m_dMinRow;
	SphDocID_t						m_iMinDocid;
	CSphFixedVector<int64_t>		m_dFieldLens;	///< total per-field lengths summed over entire indexed data, in tokens

private:

	CSphIndexProgress			m_tProgress;

	bool						LoadHitlessWords ( CSphVector<SphWordID_t> & dHitlessWords );

private:
	// searching-only, per-index
	static const int			DOCINFO_HASH_BITS	= 18;	// FIXME! make this configurable

	int64_t						m_iDocinfo;				///< my docinfo cache size
	CSphSharedBuffer<DWORD>		m_pDocinfoHash;			///< hashed ids, to accelerate lookups
	int64_t						m_iDocinfoIndex;		///< docinfo "index" entries count (each entry is 2x docinfo rows, for min/max)
	DWORD *						m_pDocinfoIndex;		///< docinfo "index", to accelerate filtering during full-scan (2x rows for each block, and 2x rows for the whole index, 1+m_uDocinfoIndex entries)

	CSphSharedBuffer<DWORD>		m_dAttrShared;			///< my docinfo cache
	CSphSharedBuffer<DWORD>		m_dMvaShared;			///< my multi-valued attrs cache
	CSphSharedBuffer<BYTE>		m_dStringShared;		///< my in-RAM strings cache

	CSphMappedBuffer<DWORD>		m_dAttrMapped;
	CSphMappedBuffer<DWORD>		m_dMvaMapped;
	CSphMappedBuffer<BYTE>		m_dStringMapped;

	CSphBufferTrait<DWORD>		m_tAttr;
	CSphBufferTrait<DWORD>		m_tMva;
	CSphBufferTrait<BYTE>		m_tString;

	bool						m_bOndiskAllAttr;
	bool						m_bOndiskPoolAttr;

	CWordlist					m_tWordlist;			///< my wordlist

	bool						m_bKeepAttrs;			///< retain attributes on reindexing

	CSphSharedBuffer<SphAttr_t>	m_pKillList;			///< killlist
	DWORD						m_iKillListSize;		///< killlist size (in elements)

	CSphSharedBuffer<BYTE>		m_pSkiplists;			///< (compressed) skiplists data

	int64_t						m_uMinMaxIndex;			///< stored min/max cache offset (counted in DWORDs)

	CSphAutofile				m_tDoclistFile;			///< doclist file
	CSphAutofile				m_tHitlistFile;			///< hitlist file

#define SPH_SHARED_VARS_COUNT 2

	DWORD *						m_pPreread;
	DWORD *						m_pAttrsStatus;
	CSphSharedBuffer<DWORD>		m_dShared;				///< are we ready to search

	bool						m_bPreallocated;		///< are we ready to preread
	DWORD						m_uVersion;				///< data files version
	bool						m_bUse64;				///< whether the header is id64
	bool						m_bHaveSkips;			///< whether we have skiplists

	int							m_iIndexTag;			///< my ids for MVA updates pool
	static int					m_iIndexTagSeq;			///< static ids sequence

	bool						m_bIsEmpty;				///< do we have actually indexed documents (m_iTotalDocuments is just fetched documents, not indexed!)

private:
	CSphString					GetIndexFileName ( const char * sExt ) const;

	bool						ParsedMultiQuery ( const CSphQuery * pQuery, CSphQueryResult * pResult, int iSorters, ISphMatchSorter ** ppSorters, const XQQuery_t & tXQ, CSphDict * pDict, const CSphMultiQueryArgs & tArgs, CSphQueryNodeCache * pNodeCache ) const;
	bool						MultiScan ( const CSphQuery * pQuery, CSphQueryResult * pResult, int iSorters, ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const;
	void						MatchExtended ( CSphQueryContext * pCtx, const CSphQuery * pQuery, int iSorters, ISphMatchSorter ** ppSorters, ISphRanker * pRanker, int iTag, int iIndexWeight ) const;

	const DWORD *				FindDocinfo ( SphDocID_t uDocID ) const;
	void						CopyDocinfo ( const CSphQueryContext * pCtx, CSphMatch & tMatch, const DWORD * pFound ) const;

	bool						BuildMVA ( const CSphVector<CSphSource*> & dSources, CSphFixedVector<CSphWordHit> & dHits, int iArenaSize, int iFieldFD, int nFieldMVAs, int iFieldMVAInPool, CSphIndex_VLN * pPrevIndex );

	bool						IsStarDict() const;
	CSphDict *					SetupStarDict ( CSphScopedPtr<CSphDict> & tContainer, CSphDict * pPrevDict ) const;
	CSphDict *					SetupExactDict ( CSphScopedPtr<CSphDict> & tContainer, CSphDict * pPrevDict ) const;

	bool						RelocateBlock ( int iFile, BYTE * pBuffer, int iRelocationSize, SphOffset_t * pFileSize, CSphBin * pMinBin, SphOffset_t * pSharedOffset );
	bool						PrecomputeMinMax();

private:
	static const int MAX_ORDINAL_STR_LEN	= 4096;	///< maximum ordinal string length in bytes
	static const int ORDINAL_READ_SIZE		= 262144;	///< sorted ordinal id read buffer size in bytes

	ESphBinRead					ReadOrdinal ( CSphBin & Reader, Ordinal_t & Ordinal );
	SphOffset_t					DumpOrdinals ( CSphWriter & Writer, CSphVector<Ordinal_t> & dOrdinals );
	bool						SortOrdinals ( const char * szToFile, int iFromFD, int iArenaSize, int iOrdinalsInPool, CSphVector< CSphVector<SphOffset_t> > & dOrdBlockSize, bool bWarnOfMem );
	bool						SortOrdinalIds ( const char * szToFile, int iFromFD, int iArenaSize, CSphVector < CSphVector < SphOffset_t > > & dOrdBlockSize, bool bWarnOfMem );

	bool						LoadPersistentMVA ( CSphString & sError );

	bool						JuggleFile ( const char* szExt, CSphString & sError, bool bNeedOrigin=true ) const;
	XQNode_t *					ExpandPrefix ( XQNode_t * pNode, CSphString & sError, CSphQueryResultMeta * pResult ) const;

	const CSphRowitem *			CopyRow ( const CSphRowitem * pDocinfo, DWORD * pTmpDocinfo, const CSphColumnInfo * pNewAttr, int iOldStride ) const;

	bool						BuildDone ( const BuildHeader_t & tBuildHeader, CSphString & sError ) const;
};

int CSphIndex_VLN::m_iIndexTagSeq = 0;

/////////////////////////////////////////////////////////////////////////////
// UTILITY FUNCTIONS
/////////////////////////////////////////////////////////////////////////////

/// indexer warning
void sphWarn ( const char * sTemplate, ... )
{
	va_list ap;
	va_start ( ap, sTemplate );
	fprintf ( stdout, "WARNING: " );
	vfprintf ( stdout, sTemplate, ap );
	fprintf ( stdout, "\n" );
	va_end ( ap );
}

//////////////////////////////////////////////////////////////////////////

static ThrottleState_t g_tThrottle;

void sphSetThrottling ( int iMaxIOps, int iMaxIOSize )
{
	g_tThrottle.m_iMaxIOps = iMaxIOps;
	g_tThrottle.m_iMaxIOSize = iMaxIOSize;
}


static inline void sphThrottleSleep ( ThrottleState_t * pState )
{
	assert ( pState );
	if ( pState->m_iMaxIOps>0 )
	{
		int64_t tmTimer = sphMicroTimer();
		int64_t tmSleep = Max ( pState->m_tmLastIOTime + 1000000/pState->m_iMaxIOps - tmTimer, 0 );
		sphSleepMsec ( (int)(tmSleep/1000) );
		pState->m_tmLastIOTime = tmTimer + tmSleep;
	}
}


bool sphWriteThrottled ( int iFD, const void * pBuf, int64_t iCount, const char * sName, CSphString & sError, ThrottleState_t * pThrottle )
{
	assert ( pThrottle );
	if ( iCount<=0 )
		return true;

	// by default, slice ios by at most 1 GB
	int iChunkSize = ( 1UL<<30 );

	// when there's a sane max_iosize (4K to 1GB), use it
	if ( pThrottle->m_iMaxIOSize>=4096 )
		iChunkSize = Min ( iChunkSize, pThrottle->m_iMaxIOSize );

	CSphIOStats * pIOStats = GetIOStats();

	// while there's data, write it chunk by chunk
	const BYTE * p = (const BYTE*) pBuf;
	while ( iCount>0 )
	{
		// wait for a timely occasion
		sphThrottleSleep ( pThrottle );

		// write (and maybe time)
		int64_t tmTimer = 0;
		if ( pIOStats )
			tmTimer = sphMicroTimer();

		int iToWrite = iChunkSize;
		if ( iCount<iChunkSize )
			iToWrite = (int)iCount;
		int iWritten = ::write ( iFD, p, iToWrite );

		if ( pIOStats )
		{
			pIOStats->m_iWriteTime += sphMicroTimer() - tmTimer;
			pIOStats->m_iWriteOps++;
			pIOStats->m_iWriteBytes += iToWrite;
		}

		// success? rinse, repeat
		if ( iWritten==iToWrite )
		{
			iCount -= iToWrite;
			p += iToWrite;
			continue;
		}

		// failure? report, bailout
		if ( iWritten<0 )
			sError.SetSprintf ( "%s: write error: %s", sName, strerror(errno) );
		else
			sError.SetSprintf ( "%s: write error: %d of %d bytes written", sName, iWritten, iToWrite );
		return false;
	}
	return true;
}


size_t sphReadThrottled ( int iFD, void * pBuf, size_t iCount, ThrottleState_t * pThrottle )
{
	assert ( pThrottle );
	if ( pThrottle->m_iMaxIOSize && int(iCount) > pThrottle->m_iMaxIOSize )
	{
		size_t nChunks = iCount / pThrottle->m_iMaxIOSize;
		size_t nBytesLeft = iCount % pThrottle->m_iMaxIOSize;

		size_t nBytesRead = 0;
		size_t iRead = 0;

		for ( size_t i=0; i<nChunks; i++ )
		{
			iRead = sphReadThrottled ( iFD, (char *)pBuf + i*pThrottle->m_iMaxIOSize, pThrottle->m_iMaxIOSize, pThrottle );
			nBytesRead += iRead;
			if ( iRead!=(size_t)pThrottle->m_iMaxIOSize )
				return nBytesRead;
		}

		if ( nBytesLeft > 0 )
		{
			iRead = sphReadThrottled ( iFD, (char *)pBuf + nChunks*pThrottle->m_iMaxIOSize, nBytesLeft, pThrottle );
			nBytesRead += iRead;
			if ( iRead!=nBytesLeft )
				return nBytesRead;
		}

		return nBytesRead;
	}

	sphThrottleSleep ( pThrottle );
	return sphRead ( iFD, pBuf, iCount );
}

void SafeClose ( int & iFD )
{
	if ( iFD>=0 )
		::close ( iFD );
	iFD = -1;
}

//////////////////////////////////////////////////////////////////////////

#if !USE_WINDOWS
char * strlwr ( char * s )
{
	while ( *s )
	{
		*s = tolower ( *s );
		s++;
	}
	return s;
}
#endif


char * sphStrMacro ( const char * sTemplate, const char * sMacro, SphDocID_t uValue )
{
	// expand macro
	char sExp[32];
	snprintf ( sExp, sizeof(sExp), DOCID_FMT, uValue );

	// calc lengths
	int iExp = strlen ( sExp );
	int iMacro = strlen ( sMacro );
	int iDelta = iExp-iMacro;

	// calc result length
	int iRes = strlen ( sTemplate );
	const char * sCur = sTemplate;
	while ( ( sCur = strstr ( sCur, sMacro ) )!=NULL )
	{
		iRes += iDelta;
		sCur++;
	}

	// build result
	char * sRes = new char [ iRes+1 ];
	char * sOut = sRes;
	const char * sLast = sTemplate;
	sCur = sTemplate;

	while ( ( sCur = strstr ( sCur, sMacro ) )!=NULL )
	{
		strncpy ( sOut, sLast, sCur-sLast ); sOut += sCur-sLast;
		strcpy ( sOut, sExp ); sOut += iExp; // NOLINT
		sCur += iMacro;
		sLast = sCur;
	}

	if ( *sLast )
		strcpy ( sOut, sLast ); // NOLINT

	assert ( (int)strlen(sRes)==iRes );
	return sRes;
}


float sphToFloat ( const char * s )
{
	if ( !s ) return 0.0f;
	return (float)strtod ( s, NULL );
}


DWORD sphToDword ( const char * s )
{
	if ( !s ) return 0;
	return strtoul ( s, NULL, 10 );
}


uint64_t sphToUint64 ( const char * s )
{
	if ( !s ) return 0;
	return strtoull ( s, NULL, 10 );
}


int64_t sphToInt64 ( const char * s )
{
	if ( !s ) return 0;
	return strtoll ( s, NULL, 10 );
}


#if USE_64BIT
#define sphToDocid sphToUint64
#else
#define sphToDocid sphToDword
#endif


#if USE_WINDOWS

bool sphLockEx ( int iFile, bool bWait )
{
	HANDLE hHandle = (HANDLE) _get_osfhandle ( iFile );
	if ( hHandle!=INVALID_HANDLE_VALUE )
	{
		OVERLAPPED tOverlapped;
		memset ( &tOverlapped, 0, sizeof ( tOverlapped ) );
		return !!LockFileEx ( hHandle, LOCKFILE_EXCLUSIVE_LOCK | ( bWait ? 0 : LOCKFILE_FAIL_IMMEDIATELY ), 0, 1, 0, &tOverlapped );
	}

	return false;
}

void sphLockUn ( int iFile )
{
	HANDLE hHandle = (HANDLE) _get_osfhandle ( iFile );
	if ( hHandle!=INVALID_HANDLE_VALUE )
	{
		OVERLAPPED tOverlapped;
		memset ( &tOverlapped, 0, sizeof ( tOverlapped ) );
		UnlockFileEx ( hHandle, 0, 1, 0, &tOverlapped );
	}
}

#else

bool sphLockEx ( int iFile, bool bWait )
{
	struct flock tLock;
	tLock.l_type = F_WRLCK;
	tLock.l_whence = SEEK_SET;
	tLock.l_start = 0;
	tLock.l_len = 0;

	int iCmd = bWait ? F_SETLKW : F_SETLK; // FIXME! check for HAVE_F_SETLKW?
	return ( fcntl ( iFile, iCmd, &tLock )!=-1 );
}


void sphLockUn ( int iFile )
{
	struct flock tLock;
	tLock.l_type = F_UNLCK;
	tLock.l_whence = SEEK_SET;
	tLock.l_start = 0;
	tLock.l_len = 0;

	fcntl ( iFile, F_SETLK, &tLock );
}
#endif


void sphSleepMsec ( int iMsec )
{
	if ( iMsec<0 )
		return;

#if USE_WINDOWS
	Sleep ( iMsec );

#else
	struct timeval tvTimeout;
	tvTimeout.tv_sec = iMsec / 1000; // full seconds
	tvTimeout.tv_usec = ( iMsec % 1000 ) * 1000; // remainder is msec, so *1000 for usec

	select ( 0, NULL, NULL, NULL, &tvTimeout ); // FIXME? could handle EINTR
#endif
}


bool sphIsReadable ( const char * sPath, CSphString * pError )
{
	int iFD = ::open ( sPath, O_RDONLY );

	if ( iFD<0 )
	{
		if ( pError )
			pError->SetSprintf ( "%s unreadable: %s", sPath, strerror(errno) );
		return false;
	}

	close ( iFD );
	return true;
}


int sphOpenFile ( const char * sFile, CSphString & sError )
{
	int iFD = ::open ( sFile, SPH_O_READ, 0644 );
	if ( iFD<0 )
	{
		sError.SetSprintf ( "failed to open file '%s': '%s'", sFile, strerror(errno) );
		return -1;
	}

	return iFD;
}


int64_t sphGetFileSize ( int iFD, CSphString & sError )
{
	if ( iFD<0 )
	{
		sError.SetSprintf ( "invalid descriptor to fstat '%d'", iFD );
		return -1;
	}

	struct_stat st;
	if ( fstat ( iFD, &st )<0 )
	{
		sError.SetSprintf ( "failed to fstat file '%d': '%s'", iFD, strerror(errno) );
		return -1;
	}

	return st.st_size;
}



void sphSetReadBuffers ( int iReadBuffer, int iReadUnhinted )
{
	if ( iReadBuffer<=0 )
		iReadBuffer = DEFAULT_READ_BUFFER;
	g_iReadBuffer = Max ( iReadBuffer, MIN_READ_BUFFER );

	if ( iReadUnhinted<=0 )
		iReadUnhinted = DEFAULT_READ_UNHINTED;
	g_iReadUnhinted = Max ( iReadUnhinted, MIN_READ_UNHINTED );
}

//////////////////////////////////////////////////////////////////////////
// DOCINFO
//////////////////////////////////////////////////////////////////////////


static DWORD *				g_pMvaArena = NULL;		///< initialized by sphArenaInit()


// OPTIMIZE! try to inline or otherwise simplify maybe
const DWORD * CSphMatch::GetAttrMVA ( const CSphAttrLocator & tLoc, const DWORD * pPool ) const
{
	DWORD uIndex = MVA_DOWNSIZE ( GetAttr ( tLoc ) );
	if ( !uIndex )
		return NULL;

	if ( uIndex & MVA_ARENA_FLAG )
		return g_pMvaArena + ( uIndex & MVA_OFFSET_MASK );

	assert ( pPool );
	return pPool + uIndex;
}

/////////////////////////////////////////////////////////////////////////////
// TOKENIZERS
/////////////////////////////////////////////////////////////////////////////

inline int sphUTF8Decode ( const BYTE * & pBuf ); // forward ref for GCC
inline int sphUTF8Encode ( BYTE * pBuf, int iCode ); // forward ref for GCC


/// synonym list entry
struct CSphSynonym
{
	CSphString	m_sFrom;	///< specially packed list of map-from tokens
	CSphString	m_sTo;		///< map-to string
	int			m_iFromLen;	///< cached m_sFrom length
	int			m_iToLen;	///< cached m_sTo length

	inline bool operator < ( const CSphSynonym & rhs ) const
	{
		return strcmp ( m_sFrom.cstr(), rhs.m_sFrom.cstr() ) < 0;
	}
};


/// base that is completely identical in both SBCS and UTF8 tokenizers
class CSphTokenizerBase : public ISphTokenizer
{
public:
	CSphTokenizerBase ();

	virtual bool			SetCaseFolding ( const char * sConfig, CSphString & sError );
	virtual bool			LoadSynonyms ( const char * sFilename, const CSphEmbeddedFiles * pFiles, CSphString & sError );
	virtual void			WriteSynonyms ( CSphWriter & tWriter );
	virtual void			CloneBase ( const CSphTokenizerBase * pFrom, ESphTokenizerClone eMode );

	virtual const char *	GetTokenStart () const		{ return (const char *) m_pTokenStart; }
	virtual const char *	GetTokenEnd () const		{ return (const char *) m_pTokenEnd; }
	virtual const char *	GetBufferPtr () const		{ return (const char *) m_pCur; }
	virtual const char *	GetBufferEnd () const		{ return (const char *) m_pBufferMax; }
	virtual void			SetBufferPtr ( const char * sNewPtr );
	virtual uint64_t		GetSettingsFNV () const;

	virtual bool			SetBlendChars ( const char * sConfig, CSphString & sError );

public:
	// lightweight clones must impose a lockdown on some methods
	// (specifically those that change the lowercaser data table)

	virtual void AddPlainChar ( char c )
	{
		assert ( m_eMode!=SPH_CLONE_QUERY_LIGHTWEIGHT );
		ISphTokenizer::AddPlainChar ( c );
	}

	virtual void AddSpecials ( const char * sSpecials )
	{
		assert ( m_eMode!=SPH_CLONE_QUERY_LIGHTWEIGHT );
		ISphTokenizer::AddSpecials ( sSpecials );
	}

	virtual void Setup ( const CSphTokenizerSettings & tSettings )
	{
		assert ( m_eMode!=SPH_CLONE_QUERY_LIGHTWEIGHT );
		ISphTokenizer::Setup ( tSettings );
	}

	virtual bool RemapCharacters ( const char * sConfig, DWORD uFlags, const char * sSource, bool bCanRemap, CSphString & sError )
	{
		assert ( m_eMode!=SPH_CLONE_QUERY_LIGHTWEIGHT );
		return ISphTokenizer::RemapCharacters ( sConfig, uFlags, sSource, bCanRemap, sError );
	}

protected:
	bool	BlendAdjust ( const BYTE * pPosition );
	int		CodepointArbitrationI ( int iCodepoint );
	int		CodepointArbitrationQ ( int iCodepoint, bool bWasEscaped, BYTE uNextByte );

	typedef CSphOrderedHash <int, int, IdentityHash_fn, 4096> CSphSynonymHash;
	bool	LoadSynonym ( char * sBuffer, const char * szFilename, int iLine, CSphSynonymHash & tHash, CSphString & sError );


protected:
	const BYTE *		m_pBuffer;							///< my buffer
	const BYTE *		m_pBufferMax;						///< max buffer ptr, exclusive (ie. this ptr is invalid, but every ptr below is ok)
	const BYTE *		m_pCur;								///< current position
	const BYTE *		m_pTokenStart;						///< last token start point
	const BYTE *		m_pTokenEnd;						///< last token end point

	BYTE				m_sAccum [ 3*SPH_MAX_WORD_LEN+3 ];	///< folded token accumulator
	BYTE *				m_pAccum;							///< current accumulator position
	int					m_iAccum;							///< boundary token size

	BYTE				m_sAccumBlend [ 3*SPH_MAX_WORD_LEN+3 ];	///< blend-acc, an accumulator copy for additional blended variants
	int					m_iBlendNormalStart;					///< points to first normal char in the accumulators (might be NULL)
	int					m_iBlendNormalEnd;						///< points just past (!) last normal char in the accumulators (might be NULL)

	CSphVector<CSphSynonym>			m_dSynonyms;				///< active synonyms
	CSphVector<int>					m_dSynStart;				///< map 1st byte to candidate range start
	CSphVector<int>					m_dSynEnd;					///< map 1st byte to candidate range end

	bool			m_bHasBlend;
	const BYTE *	m_pBlendStart;
	const BYTE *	m_pBlendEnd;

	ESphTokenizerClone	m_eMode;
};


/// methods taht get specialized with regards to charset type
/// aka GetCodepoint() decoder and everything that depends on it
template < bool IS_UTF8 >
class CSphTokenizerBase2 : public CSphTokenizerBase
{
protected:
	/// get codepoint
	inline int GetCodepoint ()
	{
		if_const ( IS_UTF8 )
		{
			while ( m_pCur<m_pBufferMax )
			{
				int iCode = sphUTF8Decode ( m_pCur );
				if ( iCode>=0 )
					return iCode; // succesful decode
			}
			return -1; // eof
		} else
		{
			return m_pCur>=m_pBufferMax
				? -1
				: int ( *m_pCur++ );
		}
	}

	/// accum codepoint
	inline void AccumCodepoint ( int iCode )
	{
		assert ( iCode>0 );
		assert ( m_iAccum>=0 );

		// throw away everything which is over the token size
		bool bFit = ( m_iAccum<SPH_MAX_WORD_LEN );
		if_const ( IS_UTF8 )
			bFit &= ( m_pAccum-m_sAccum+SPH_MAX_UTF8_BYTES<=(int)sizeof(m_sAccum));

		if ( bFit )
		{
			if_const ( IS_UTF8 )
				m_pAccum += sphUTF8Encode ( m_pAccum, iCode );
			else
				*m_pAccum++ = BYTE(iCode);
			assert ( m_pAccum>=m_sAccum && m_pAccum<m_sAccum+sizeof(m_sAccum) );

			m_iAccum++;
		}
	}

protected:
	BYTE *			GetTokenSyn ( bool bQueryMode );
	BYTE *			GetBlendedVariant ();

public:
	virtual int		SkipBlended ();
};


/// single-byte charset tokenizer
template < bool IS_QUERY >
class CSphTokenizer_SBCS : public CSphTokenizerBase2<false>
{
public:
								CSphTokenizer_SBCS ();

	virtual void				SetBuffer ( const BYTE * sBuffer, int iLength );
	virtual BYTE *				GetToken ();
	virtual ISphTokenizer *		Clone ( ESphTokenizerClone eMode ) const;
	virtual bool				IsUtf8 () const { return false; }
	virtual int					GetCodepointLength ( int ) const { return 1; }
	virtual int					GetMaxCodepointLength () const { return 1; }
};


/// templated UTF-8 implementation of GetToken
class CSphTokenizer_UTF8_Base : public CSphTokenizerBase2<true>
{
protected:
	template < bool IS_QUERY, bool IS_BLEND >
	BYTE *						DoGetToken();

	void						FlushAccum ();
};


/// UTF-8 tokenizer
template < bool IS_QUERY >
class CSphTokenizer_UTF8 : public CSphTokenizer_UTF8_Base
{
public:
								CSphTokenizer_UTF8 ();
	virtual void				SetBuffer ( const BYTE * sBuffer, int iLength );
	virtual BYTE *				GetToken ();
	virtual ISphTokenizer *		Clone ( ESphTokenizerClone eMode ) const;
	virtual bool				IsUtf8 () const { return true; }
	virtual int					GetCodepointLength ( int iCode ) const;
	virtual int					GetMaxCodepointLength () const { return m_tLC.GetMaxCodepointLength(); }
};

/// UTF-8 Chinese tokenizer
template < bool IS_QUERY >
class CSphTokenizer_UTF8Chinese : public CSphTokenizer_UTF8_Base
{
public:
                                                                CSphTokenizer_UTF8Chinese ();
        virtual bool                            SetChineseDictionary ( const char * sConfig, CSphString & sError );
        virtual void                            SetBuffer ( const BYTE * sBuffer, int iLength );
        virtual BYTE *                          GetToken ();
        virtual void                                            FlushAccum ();
        virtual ISphTokenizer *         Clone ( ESphTokenizerClone eMode ) const;
        virtual bool                            IsUtf8 () const { return true; }
        virtual int                                     GetCodepointLength ( int iCode ) const;
        virtual int                                     GetMaxCodepointLength () const { return m_tLC.GetMaxCodepointLength(); }
        

        int                                     getBestWordLength(Darts::DoubleArray::key_type *pText, size_t iTextLength);
        Darts::DoubleArray                      m_tDa;
        Darts::DoubleArray::key_type            *m_pText;
        Darts::DoubleArray::result_pair_type    m_pResultPair[256];
};

/// UTF-8 tokenizer with n-grams
template < bool IS_QUERY >
class CSphTokenizer_UTF8Ngram : public CSphTokenizer_UTF8<IS_QUERY>
{
public:
						CSphTokenizer_UTF8Ngram () : m_iNgramLen ( 1 ) {}

public:
	virtual bool		SetNgramChars ( const char * sConfig, CSphString & sError );
	virtual void		SetNgramLen ( int iLen );
	virtual BYTE *		GetToken ();

protected:
	int					m_iNgramLen;
	CSphString			m_sNgramCharsStr;
};


struct CSphMultiform
{
	CSphString				m_sNormalForm;
	int						m_iNormalTokenLen;
	CSphVector<CSphString>	m_dTokens;
};


struct CSphMultiforms
{
	int							m_iMinTokens;
	int							m_iMaxTokens;
	CSphVector<CSphMultiform*>	m_pForms;		// OPTIMIZE? blobify?
};


struct CSphMultiformContainer
{
							CSphMultiformContainer () : m_iMaxTokens ( 0 ) {}

	int						m_iMaxTokens;
	typedef CSphOrderedHash < CSphMultiforms *, CSphString, CSphStrHashFunc, 131072 > CSphMultiformHash;
	CSphMultiformHash	m_Hash;
};


/// token filter for multiforms support
class CSphMultiformTokenizer : public CSphTokenFilter
{
public:
	CSphMultiformTokenizer ( ISphTokenizer * pTokenizer, const CSphMultiformContainer * pContainer );
	~CSphMultiformTokenizer ();

	virtual bool					SetCaseFolding ( const char * sConfig, CSphString & sError )	{ return m_pTokenizer->SetCaseFolding ( sConfig, sError ); }
	virtual void					AddPlainChar ( char c )											{ m_pTokenizer->AddPlainChar ( c ); }
	virtual void					AddSpecials ( const char * sSpecials )							{ m_pTokenizer->AddSpecials ( sSpecials ); }
	virtual bool					SetIgnoreChars ( const char * sIgnored, CSphString & sError )	{ return m_pTokenizer->SetIgnoreChars ( sIgnored, sError ); }
	virtual bool					SetNgramChars ( const char * sConfig, CSphString & sError )		{ return m_pTokenizer->SetNgramChars ( sConfig, sError ); }
	virtual void					SetNgramLen ( int iLen )										{ m_pTokenizer->SetNgramLen ( iLen ); }
	virtual bool					LoadSynonyms ( const char * sFilename, const CSphEmbeddedFiles * pFiles, CSphString & sError ) { return m_pTokenizer->LoadSynonyms ( sFilename, pFiles, sError ); }
	virtual bool					SetBoundary ( const char * sConfig, CSphString & sError )		{ return m_pTokenizer->SetBoundary ( sConfig, sError ); }
	virtual void					Setup ( const CSphTokenizerSettings & tSettings )				{ m_pTokenizer->Setup ( tSettings ); }
	virtual const CSphTokenizerSettings &	GetSettings () const									{ return m_pTokenizer->GetSettings (); }
	virtual const CSphSavedFile &	GetSynFileInfo () const											{ return m_pTokenizer->GetSynFileInfo (); }
	virtual bool					EnableSentenceIndexing ( CSphString & sError )					{ return m_pTokenizer->EnableSentenceIndexing ( sError ); }
	virtual bool					EnableZoneIndexing ( CSphString & sError )						{ return m_pTokenizer->EnableZoneIndexing ( sError ); }

public:
	virtual void					SetBuffer ( const BYTE * sBuffer, int iLength );
	virtual BYTE *					GetToken ();
	virtual void					EnableTokenizedMultiformTracking ()			{ m_bBuildMultiform = true; }
	virtual int						GetLastTokenLen () const					{ return m_iStart<m_dStoredTokens.GetLength() ? m_dStoredTokens[m_iStart].m_iTokenLen : m_pTokenizer->GetLastTokenLen(); }
	virtual bool					GetBoundary ()								{ return m_iStart<m_dStoredTokens.GetLength() ? m_dStoredTokens[m_iStart].m_bBoundary : m_pTokenizer->GetBoundary(); }
	virtual bool					WasTokenSpecial ()							{ return m_iStart<m_dStoredTokens.GetLength() ? m_dStoredTokens[m_iStart].m_bSpecial : m_pTokenizer->WasTokenSpecial(); }
	virtual int						GetOvershortCount ()						{ return m_iStart<m_dStoredTokens.GetLength() ? m_dStoredTokens[m_iStart].m_iOvershortCount : m_pTokenizer->GetOvershortCount(); }
	virtual BYTE *					GetTokenizedMultiform ()					{ return m_sTokenizedMultiform[0] ? m_sTokenizedMultiform : NULL; }
	virtual bool					TokenIsBlended () const						{ return m_iStart<m_dStoredTokens.GetLength() ? m_dStoredTokens[m_iStart].m_bBlended : m_pTokenizer->TokenIsBlended(); }
	virtual bool					TokenIsBlendedPart () const					{ return m_iStart<m_dStoredTokens.GetLength() ? m_dStoredTokens[m_iStart].m_bBlendedPart : m_pTokenizer->TokenIsBlendedPart(); }
	virtual int						SkipBlended ();

public:
	virtual ISphTokenizer *			Clone ( ESphTokenizerClone eMode ) const;
	virtual bool					IsUtf8 () const				{ return m_pTokenizer->IsUtf8 (); }
	virtual const char *			GetTokenStart () const		{ return m_iStart<m_dStoredTokens.GetLength() ? m_dStoredTokens[m_iStart].m_szTokenStart : m_pTokenizer->GetTokenStart(); }
	virtual const char *			GetTokenEnd () const		{ return m_iStart<m_dStoredTokens.GetLength() ? m_dStoredTokens[m_iStart].m_szTokenEnd : m_pTokenizer->GetTokenEnd(); }
	virtual const char *			GetBufferPtr () const		{ return m_iStart<m_dStoredTokens.GetLength() ? m_dStoredTokens[m_iStart].m_pBufferPtr : m_pTokenizer->GetBufferPtr(); }
	virtual void					SetBufferPtr ( const char * sNewPtr );
	virtual uint64_t				GetSettingsFNV () const;

private:
	const CSphMultiformContainer *	m_pMultiWordforms;
	int								m_iStart;

	bool				m_bBuildMultiform;
	BYTE				m_sTokenizedMultiform [ 3*SPH_MAX_WORD_LEN+4 ];

	CSphVector<StoredToken_t>		m_dStoredTokens;
};


/// token filter for bigram indexing
///
/// passes tokens through until an eligible pair is found
/// then buffers and returns that pair as a blended token
/// then returns the first token as a regular one
/// then pops the first one and cycles again
///
/// pair (aka bigram) eligibility depends on bigram_index value
/// "all" means that all token pairs gets indexed
/// "first_freq" means that 1st token must be from bigram_freq_words
/// "both_freq" means that both tokens must be from bigram_freq_words
class CSphBigramTokenizer : public CSphTokenFilter
{
protected:
	enum
	{
		BIGRAM_CLEAN,	///< clean slate, nothing accumulated
		BIGRAM_PAIR,	///< just returned a pair from m_sBuf, and m_iFirst/m_pSecond are correct
		BIGRAM_FIRST	///< just returned a first token from m_sBuf, so m_iFirst/m_pSecond are still good
	}		m_eState;
	BYTE	m_sBuf [ MAX_KEYWORD_BYTES ];	///< pair buffer
	BYTE *	m_pSecond;						///< second token pointer
	int		m_iFirst;						///< first token length, bytes

	ESphBigram			m_eMode;			///< bigram indexing mode
	int					m_iMaxLen;			///< max bigram_freq_words length
	int					m_dWordsHash[256];	///< offsets into m_dWords hashed by 1st byte
	CSphVector<BYTE>	m_dWords;			///< case-folded, sorted bigram_freq_words

public:
	CSphBigramTokenizer ( ISphTokenizer * pTok, ESphBigram eMode, CSphVector<CSphString> & dWords )
		: CSphTokenFilter ( pTok )
	{
		assert ( pTok );
		assert ( eMode!=SPH_BIGRAM_NONE );
		assert ( eMode==SPH_BIGRAM_ALL || dWords.GetLength() );

		m_sBuf[0] = 0;
		m_pSecond = NULL;
		m_eState = BIGRAM_CLEAN;
		memset ( m_dWordsHash, 0, sizeof(m_dWordsHash) );

		m_eMode = eMode;
		m_iMaxLen = 0;

		// only keep unique, real, short enough words
		dWords.Uniq();
		ARRAY_FOREACH ( i, dWords )
		{
			int iLen = Min ( dWords[i].Length(), 255 );
			if ( !iLen )
				continue;
			m_iMaxLen = Max ( m_iMaxLen, iLen );

			// hash word blocks by the first letter
			BYTE uFirst = *(BYTE*)( dWords[i].cstr() );
			if ( !m_dWordsHash [ uFirst ] )
			{
				m_dWords.Add ( 0 ); // end marker for the previous block
				m_dWordsHash [ uFirst ] = m_dWords.GetLength(); // hash new block
			}

			// store that word
			int iPos = m_dWords.GetLength();
			m_dWords.Resize ( iPos+iLen+1 );

			m_dWords[iPos] = (BYTE)iLen;
			memcpy ( &m_dWords [ iPos+1 ], dWords[i].cstr(), iLen );
		}
		m_dWords.Add ( 0 );
	}

	CSphBigramTokenizer ( ISphTokenizer * pTok, const CSphBigramTokenizer * pBase )
		: CSphTokenFilter ( pTok )
	{
		m_sBuf[0] = 0;
		m_pSecond = NULL;
		m_eState = BIGRAM_CLEAN;
		m_eMode = pBase->m_eMode;
		m_iMaxLen = pBase->m_iMaxLen;
		memcpy ( m_dWordsHash, pBase->m_dWordsHash, sizeof(m_dWordsHash) );
		m_dWords = pBase->m_dWords;
	}

	ISphTokenizer * Clone ( ESphTokenizerClone eMode ) const
	{
		ISphTokenizer * pTok = m_pTokenizer->Clone ( eMode );
		return new CSphBigramTokenizer ( pTok, this );
	}

	void SetBuffer ( const BYTE * sBuffer, int iLength )
	{
		m_pTokenizer->SetBuffer ( sBuffer, iLength );
	}

	bool TokenIsBlended() const
	{
		if ( m_eState==BIGRAM_PAIR )
			return true;
		if ( m_eState==BIGRAM_FIRST )
			return false;
		return m_pTokenizer->TokenIsBlended();
	}

	bool IsFreq ( int iLen, BYTE * sWord )
	{
		// early check
		if ( iLen>m_iMaxLen )
			return false;

		// hash lookup, then linear scan
		int iPos = m_dWordsHash [ *sWord ];
		if ( !iPos )
			return false;
		while ( m_dWords[iPos] )
		{
			if ( m_dWords[iPos]==iLen && !memcmp ( sWord, &m_dWords[iPos+1], iLen ) )
				break;
			iPos += 1+m_dWords[iPos];
		}
		return m_dWords[iPos]!=0;
	}

	BYTE * GetToken()
	{
		if ( m_eState==BIGRAM_FIRST || m_eState==BIGRAM_CLEAN )
		{
			BYTE * pFirst;
			if ( m_eState==BIGRAM_FIRST )
			{
				// first out, clean slate again, actually
				// and second will now become our next first
				assert ( m_pSecond );
				m_eState = BIGRAM_CLEAN;
				pFirst = m_pSecond;
				m_pSecond = NULL;
			} else
			{
				// just clean slate
				// assure we're, well, clean
				assert ( !m_pSecond );
				pFirst = m_pTokenizer->GetToken();
			}

			// clean slate
			// get first non-blended token
			if ( !pFirst )
				return NULL;

			// pass through blended
			// could handle them as first too, but.. cumbersome
			if ( m_pTokenizer->TokenIsBlended() )
				return pFirst;

			// check pair
			// in first_freq and both_freq modes, 1st token must be listed
			m_iFirst = strlen ( (const char*)pFirst );
			if ( m_eMode!=SPH_BIGRAM_ALL && !IsFreq ( m_iFirst, pFirst ) )
					return pFirst;

			// copy it
			// subsequent calls can and will override token accumulator
			memcpy ( m_sBuf, pFirst, m_iFirst+1 );

			// grow a pair!
			// get a second one (lookahead, in a sense)
			BYTE * pSecond = m_pTokenizer->GetToken();

			// eof? oi
			if ( !pSecond )
				return m_sBuf;

			// got a pair!
			// check combined length
			m_pSecond = pSecond;
			int iSecond = strlen ( (const char*)pSecond );
			if ( m_iFirst+iSecond+1 > SPH_MAX_WORD_LEN )
			{
				// too long pair
				// return first token as is
				m_eState = BIGRAM_FIRST;
				return m_sBuf;
			}

			// check pair
			// in freq2 mode, both tokens must be listed
			if ( m_eMode==SPH_BIGRAM_BOTHFREQ && !IsFreq ( iSecond, m_pSecond ) )
			{
				m_eState = BIGRAM_FIRST;
				return m_sBuf;
			}

			// ok, this is a eligible pair
			// begin with returning first+second pair (as blended)
			m_eState = BIGRAM_PAIR;
			m_sBuf [ m_iFirst ] = MAGIC_WORD_BIGRAM;
			assert ( m_iFirst + strlen ( (const char*)pSecond ) < sizeof(m_sBuf) );
			strcpy ( (char*)m_sBuf+m_iFirst+1, (const char*)pSecond ); //NOLINT
			return m_sBuf;

		} else if ( m_eState==BIGRAM_PAIR )
		{
			// pair (aka bigram) out, return first token as a regular token
			m_eState = BIGRAM_FIRST;
			m_sBuf [ m_iFirst ] = 0;
			return m_sBuf;
		}

		assert ( 0 && "unhandled bigram tokenizer internal state" );
		return NULL;
	}

	uint64_t GetSettingsFNV () const
	{
		uint64_t uHash = CSphTokenFilter::GetSettingsFNV();
		uHash = sphFNV64 ( m_dWords.Begin(), m_dWords.GetLength(), uHash );
		return uHash;
	}
};

/////////////////////////////////////////////////////////////////////////////

void FillStoredTokenInfo ( StoredToken_t & tToken, const BYTE * sToken, ISphTokenizer * pTokenizer )
{
	assert ( sToken && pTokenizer );
	strncpy ( (char *)tToken.m_sToken, (const char *)sToken, sizeof(tToken.m_sToken) );
	tToken.m_szTokenStart = pTokenizer->GetTokenStart ();
	tToken.m_szTokenEnd = pTokenizer->GetTokenEnd ();
	tToken.m_iOvershortCount = pTokenizer->GetOvershortCount ();
	tToken.m_iTokenLen = pTokenizer->GetLastTokenLen ();
	tToken.m_pBufferPtr = pTokenizer->GetBufferPtr ();
	tToken.m_bBoundary = pTokenizer->GetBoundary ();
	tToken.m_bSpecial = pTokenizer->WasTokenSpecial ();
	tToken.m_bBlended = pTokenizer->TokenIsBlended();
	tToken.m_bBlendedPart = pTokenizer->TokenIsBlendedPart();
}


#if USE_RLP

BT_RLP_EnvironmentC * g_pRLPEnv = NULL;
int g_iRLPEnvRefCount = 0;

static void RLPLog ( void *, int iChannel, const char * szMessage )
{
	switch ( iChannel )
	{
	case 0:
		sphWarning ( "%s", szMessage );
		break;

	case 1:
		sphLogFatal ( "%s", szMessage );
		break;

	default:
		sphInfo ( "%s", szMessage );
		break;
	}
}

static bool sphRLPInit ( const char * szRootPath, const char * szEnvPath, CSphString & sError )
{
	if ( !g_pRLPEnv )
	{
		if ( !BT_RLP_CLibrary_VersionIsCompatible ( BT_RLP_CLIBRARY_INTERFACE_VERSION ) )
		{
			sError.SetSprintf ( "RLP library mismatch: have %ld expect %d", BT_RLP_CLibrary_VersionNumber(), BT_RLP_CLIBRARY_INTERFACE_VERSION );
			return false;
		}

		BT_RLP_Environment_SetBTRootDirectory ( szRootPath );
		BT_RLP_Environment_SetLogCallbackFunction ( NULL, RLPLog );
		BT_RLP_Environment_SetLogLevel ( "error" );

		g_pRLPEnv = BT_RLP_Environment_Create();
		if ( !g_pRLPEnv )
		{
			sError = "Unable to initialize RLP environment";
			return false;
		}

		BT_Result iRes = BT_RLP_Environment_InitializeFromFile ( g_pRLPEnv, szEnvPath );
		if ( iRes!=BT_OK )
		{
			sError = "Unable to initialize the RLP environment";
			BT_RLP_Environment_Destroy ( g_pRLPEnv );
			g_pRLPEnv = NULL;
			return false;
		}
	}

	g_iRLPEnvRefCount++;
	return true;
}


static void sphRLPFree ()
{
	g_iRLPEnvRefCount--;
	if ( !g_iRLPEnvRefCount )
	{
		assert ( g_pRLPEnv );
		BT_RLP_Environment_Destroy ( g_pRLPEnv );
		g_pRLPEnv = NULL;
	}
}


class CSphRLPTokenizer : public CSphTokenFilter
{
public:
	CSphRLPTokenizer ( ISphTokenizer * pTok, const char * szRootPath, const char * szEnvPath, const char * szCtxPath, bool bStandalone )
		: CSphTokenFilter ( pTok )
		, m_pContext ( NULL )
		, m_pFactory ( NULL )
		, m_pTokenIterator ( NULL )
		, m_sRootPath ( szRootPath )
		, m_sEnvPath ( szEnvPath )
		, m_sCtxPath ( szCtxPath )
		, m_bChineseBuffer ( false )
		, m_bStandalone ( bStandalone )
		, m_bInitialized ( false )
		, m_bCloned ( false )
		, m_iLargeSegmentOffset ( 0 )
		, m_iCurNonChineseToken ( 0 )
		, m_bNonChineseToken ( false )
	{
		assert ( pTok );
		sphUTF8Encode ( m_pMarkerTokenSeparator, PROXY_TOKEN_SEPARATOR );
	}

	virtual ~CSphRLPTokenizer()
	{
		if ( !m_bCloned )
		{
			if ( m_pTokenIterator )
				BT_RLP_TokenIterator_Destroy ( m_pTokenIterator );

			if ( m_pFactory )
				BT_RLP_TokenIteratorFactory_Destroy ( m_pFactory );

			if ( m_pContext )
				BT_RLP_Environment_DestroyContext ( g_pRLPEnv, m_pContext );

			sphRLPFree();
		}
	}

	bool Init ( CSphString & sError )
	{
		assert ( !m_bInitialized && !m_bCloned );

		if ( !sphRLPInit ( m_sRootPath.cstr(), m_sEnvPath.cstr(), sError ) )
			return false;

		assert ( g_pRLPEnv );

		BT_Result iRes = BT_RLP_Environment_GetContextFromFile ( g_pRLPEnv, m_sCtxPath.cstr(), &m_pContext );
		if ( iRes!=BT_OK )
		{
			sError = "Unable to create RLP context";
			return false;
		}

		m_pFactory = BT_RLP_TokenIteratorFactory_Create();
		if ( !m_pFactory )
		{
			sError = "Unable to create RLP token iterator factory";
			return false;
		}

		m_bInitialized = true;

		return true;
	}

	virtual ISphTokenizer * Clone ( ESphTokenizerClone eMode ) const
	{
		ISphTokenizer * pTok = m_pTokenizer->Clone ( eMode );

		CSphRLPTokenizer * pClone;
		if ( eMode==SPH_CLONE_QUERY_LIGHTWEIGHT )
		{
			pClone = new CSphRLPTokenizer ( pTok, m_sRootPath.cstr(), m_sEnvPath.cstr(), m_sCtxPath.cstr(), m_bStandalone );
			pClone->m_pContext = m_pContext;
			pClone->m_pFactory = m_pFactory;
			pClone->m_pTokenIterator = m_pTokenIterator;
			pClone->m_bStandalone = m_bStandalone;
			pClone->m_bInitialized = true;
			pClone->m_bCloned = true;
		} else
		{
			pClone = new CSphRLPTokenizer ( pTok, m_sRootPath.cstr(), m_sEnvPath.cstr(), m_sCtxPath.cstr(), m_bStandalone );
			CSphString sError;
			Verify ( pClone->Init ( sError ) );
		}

		return pClone;
	}

	virtual void SetBuffer ( const BYTE * szBuffer, int iBufferLength )
	{
		assert ( m_bInitialized && g_pRLPEnv && m_pContext && m_pFactory );

		m_bChineseBuffer = m_bStandalone ? sphDetectChinese ( szBuffer, iBufferLength ) : true;
		if ( m_bChineseBuffer )
		{
			int iLength = iBufferLength;
			if ( iBufferLength>=MAX_CHUNK_SIZE )
			{
				m_dLargeBuffer.Resize ( iLength );
				memcpy ( m_dLargeBuffer.Begin(), szBuffer, iBufferLength );
				iLength = GetNextLengthToSegment ( szBuffer, iBufferLength );
				m_iLargeSegmentOffset = iLength;
			}

			if ( m_bStandalone )
			{
				m_dDocBuffer.Resize(0);
				m_dNonChineseTokens.Resize(0);
				m_iCurNonChineseToken = 0;

				m_pTokenizer->SetBuffer ( szBuffer, iBufferLength );

				BYTE * pToken;
				while ( ( pToken = m_pTokenizer->GetToken() )!=NULL )
				{
					int iTokenLen = strlen ( (const char*)pToken );

					int iOldBufferLen = m_dDocBuffer.GetLength();
					m_dDocBuffer.Resize ( iOldBufferLen+Max ( PROXY_MARKER_LEN+1, iTokenLen )+1 );
					BYTE * pCurDocPtr = &(m_dDocBuffer[iOldBufferLen]);

					if ( sphDetectChinese ( pToken, iTokenLen ) )
					{
						// collect it in one big chinese token buffer that will be processed by RLP
						memcpy ( pCurDocPtr, pToken, iTokenLen );
						pCurDocPtr += iTokenLen;
					} else
					{
						StoredToken_t & tStored = m_dNonChineseTokens.Add();
						FillStoredTokenInfo ( tStored, pToken, m_pTokenizer );

						// fixup a couple of fields
						tStored.m_pBufferPtr = m_pTokenizer->GetBufferPtr();
						tStored.m_szTokenStart = (const char *)tStored.m_sToken;
						tStored.m_szTokenEnd = (const char *)tStored.m_sToken+iTokenLen;

						// add a 'non-chinese token' marker to the chinese token stream
						*pCurDocPtr++ = ' ';
						COPY_MARKER ( pCurDocPtr, m_pMarkerTokenSeparator );
					}

					*pCurDocPtr++ = ' ';

					m_dDocBuffer.Resize ( pCurDocPtr-m_dDocBuffer.Begin() );
				}

				ProcessBufferRLP ( m_dDocBuffer.Begin(), m_dDocBuffer.GetLength() );
			} else
				ProcessBufferRLP ( szBuffer, iBufferLength );
		} else
			m_pTokenizer->SetBuffer ( szBuffer, iBufferLength );
	}

	virtual BYTE * GetToken()
	{
		assert ( m_bInitialized && g_pRLPEnv && m_pContext && m_pFactory );

		if ( m_bChineseBuffer )
		{
			m_bNonChineseToken = false;
			BYTE * pToken = GetNextTokenRLP();
			if ( pToken )
			{
				if ( m_bStandalone )
				{
					int iTokenLen = strlen ( (const char *)pToken );
					if ( iTokenLen==PROXY_MARKER_LEN && CMP_MARKER ( pToken, m_pMarkerTokenSeparator ) )
					{
						m_bNonChineseToken = true;
						return m_dNonChineseTokens[m_iCurNonChineseToken++].m_sToken;
					}
				}

				return pToken;
			}

			DestroyIteratorRLP();

			while ( m_dLargeBuffer.GetLength() )
			{
				BYTE * pBuffer = m_dLargeBuffer.Begin()+m_iLargeSegmentOffset;
				int iBufferLength = m_dLargeBuffer.GetLength()-m_iLargeSegmentOffset;
				int iLength = GetNextLengthToSegment ( pBuffer, iBufferLength );
				if ( iLength )
				{
					ProcessBufferRLP ( pBuffer, iLength );
					m_iLargeSegmentOffset += iLength;

					pToken = GetNextTokenRLP();
					if ( pToken )
						return pToken;
					else
						DestroyIteratorRLP();
				} else
				{
					m_dLargeBuffer.Resize(0);
					m_iLargeSegmentOffset = 0;
				}
			}

			return NULL;
		} else
			return m_pTokenizer->GetToken();
	}

	virtual bool GetMorphFlag () const
	{
		if ( !m_bChineseBuffer )
			return true;

		return m_bNonChineseToken;
	}

	virtual int GetLastTokenLen() const
	{
		if ( m_bChineseBuffer )
		{
			if ( m_bNonChineseToken )
				return m_dNonChineseTokens[m_iCurNonChineseToken-1].m_iTokenLen;

			return sphUTF8Len ( (const char*)m_dUTF8Buffer, sizeof ( m_dUTF8Buffer ) );
		}

		return m_pTokenizer->GetLastTokenLen();
	}

	virtual bool GetBoundary()
	{
		if ( m_bChineseBuffer )
		{
			if ( m_bNonChineseToken )
				return m_dNonChineseTokens[m_iCurNonChineseToken-1].m_bBoundary;

			return false;
		}

		return m_pTokenizer->GetBoundary();
	}

	virtual bool WasTokenSpecial()
	{
		if ( m_bChineseBuffer )
		{
			if ( m_bNonChineseToken )
				return m_dNonChineseTokens[m_iCurNonChineseToken-1].m_bSpecial;

			return false;
		}

		return m_pTokenizer->WasTokenSpecial();
	}

	virtual int	GetOvershortCount ()
	{
		if ( m_bChineseBuffer )
		{
			if ( m_bNonChineseToken )
				return m_dNonChineseTokens[m_iCurNonChineseToken-1].m_iOvershortCount;

			return 0;
		}

		return m_pTokenizer->GetOvershortCount();
	}

	virtual bool TokenIsBlended () const
	{
		if ( m_bChineseBuffer )
		{
			if ( m_bNonChineseToken )
				return m_dNonChineseTokens[m_iCurNonChineseToken-1].m_bBlended;

			return false;
		}

		return m_pTokenizer->TokenIsBlended();
	}

	virtual bool TokenIsBlendedPart () const
	{
		if ( m_bChineseBuffer )
		{
			if ( m_bNonChineseToken )
				return m_dNonChineseTokens[m_iCurNonChineseToken-1].m_bBlendedPart;

			return false;
		}

		return m_pTokenizer->TokenIsBlendedPart();
	}

	virtual const char * GetTokenStart () const
	{
		if ( m_bChineseBuffer )
		{
			if ( m_bNonChineseToken )
				return m_dNonChineseTokens[m_iCurNonChineseToken-1].m_szTokenStart;

			return (const char*)m_dUTF8Buffer;
		}

		return m_pTokenizer->GetTokenStart();
	}

	virtual const char * GetTokenEnd () const
	{
		if ( m_bChineseBuffer )
		{
			if ( m_bNonChineseToken )
				return m_dNonChineseTokens[m_iCurNonChineseToken-1].m_szTokenEnd;

			return (const char*)m_dUTF8Buffer + strlen ( (const char*)m_dUTF8Buffer );
		}

		return m_pTokenizer->GetTokenEnd();
	}

	virtual void SetBufferPtr ( const char * sNewPtr )
	{
		// we'll handle it as a complete re-tokenization
		DestroyIteratorRLP ();
		m_iCurNonChineseToken = 0;
		m_bNonChineseToken = false;
		SetBuffer ( (const BYTE*)sNewPtr, strlen ( sNewPtr ) );
	}

	virtual const char * GetBufferPtr () const
	{
		if ( m_bChineseBuffer )
		{
			if ( m_bNonChineseToken )
				return m_dNonChineseTokens[m_iCurNonChineseToken-1].m_pBufferPtr;

			return (const char*)m_dUTF8Buffer;
		}

		return m_pTokenizer->GetBufferPtr();
	}

	virtual const char * GetBufferEnd () const
	{
		if ( m_bChineseBuffer )
		{
			if ( m_bNonChineseToken )
				return m_pTokenizer->GetBufferEnd();

			return (const char*)m_dUTF8Buffer;
		}

		return m_pTokenizer->GetBufferEnd ();
	}

	virtual const char * GetRLPContext () const
	{
		return m_sCtxPath.cstr();
	}

private:
	BT_RLP_ContextC *		m_pContext;
	BT_RLP_TokenIteratorFactoryC * m_pFactory;
	BT_RLP_TokenIteratorC *	m_pTokenIterator;
	CSphString				m_sRootPath;
	CSphString				m_sEnvPath;
	CSphString				m_sCtxPath;
	bool					m_bChineseBuffer;
	bool					m_bStandalone;
	bool					m_bInitialized;
	bool					m_bCloned;
	static const int		MAX_CHUNK_SIZE = 10485760;
	static const int		MAX_TOKEN_LEN = 1024;
	CSphTightVector<BYTE>	m_dLargeBuffer;
	CSphVector<BYTE>		m_dDocBuffer;
	CSphTightVector<StoredToken_t> m_dNonChineseTokens;
	int						m_iLargeSegmentOffset;
	int						m_iCurNonChineseToken;
	bool					m_bNonChineseToken;
	BYTE					m_dUTF8Buffer[MAX_TOKEN_LEN];
	BYTE					m_pMarkerTokenSeparator[PROXY_MARKER_LEN];

	bool IsSpecialCode ( int iCode ) const
	{
		return iCode==PROXY_DOCUMENT_START || iCode==PROXY_FIELD_START_CHINESE || iCode==PROXY_FIELD_START_NONCHINESE || iCode==PROXY_TOKENIZED;
	}

	bool IsChineseSeparator ( int iCode ) const
	{
		assert ( m_pTokenizer );
		return ( iCode>=0x3000 && iCode<=0x303F ) || IsSpecialCode ( iCode ) || !m_pTokenizer->GetLowercaser().ToLower ( iCode );
	}

	inline bool IsJunkPOS ( const char * szPOS ) const
	{
		// drop EOS and PUNCT
		return !szPOS || !*szPOS || ( *szPOS=='E' && *(szPOS+1)=='O' ) || ( *szPOS=='P' && *(szPOS+1)=='U' );
	}

	void ProcessBufferRLP ( const BYTE * pBuffer, int iLength )
	{
		assert ( !m_pTokenIterator );

		BT_Result iRes = BT_RLP_Context_ProcessBuffer ( m_pContext, pBuffer, iLength, BT_LANGUAGE_SIMPLIFIED_CHINESE, "UTF-8", NULL );
		// iteration should still work ok in this case
		if ( iRes!=BT_OK )
			sphWarning ( "BT_RLP_Context_ProcessBuffer error" );

		m_pTokenIterator = BT_RLP_TokenIteratorFactory_CreateIterator ( m_pFactory, m_pContext );
		if ( !m_pTokenIterator )
			sphWarning ( "BT_RLP_TokenIteratorFactory_CreateIterator error" );
	}

	BYTE * GetNextTokenRLP()
	{
		if ( !m_pTokenIterator )
			return NULL;

		while ( BT_RLP_TokenIterator_Next ( m_pTokenIterator ) )
		{
			const BT_Char16 * pToken = BT_RLP_TokenIterator_GetToken ( m_pTokenIterator );
			const char * szPartOfSpeech = BT_RLP_TokenIterator_GetPartOfSpeech ( m_pTokenIterator );
			assert ( pToken );

			if ( IsJunkPOS ( szPartOfSpeech ) )
				continue;

			bt_xutf16toutf8 ( (char*)m_dUTF8Buffer, pToken, sizeof(m_dUTF8Buffer) );

			return &(m_dUTF8Buffer[0]);
		}

		return NULL;
	}

	void DestroyIteratorRLP()
	{
		if ( m_pTokenIterator )
		{
			BT_RLP_TokenIterator_Destroy ( m_pTokenIterator );
			m_pTokenIterator = NULL;
		}

		if ( m_pContext )
			BT_RLP_Context_DestroyResultStorage ( m_pContext );
	}

	int GetNextLengthToSegment ( const BYTE * pBuffer, int iLength ) const
	{
		const BYTE * pCurBuf = pBuffer;
		const BYTE * pLastSeparator = NULL;
		int iLengthLeft = Min ( iLength, MAX_CHUNK_SIZE );
		while ( pCurBuf<pBuffer+iLengthLeft )
		{
			int iCode = sphUTF8Decode ( pCurBuf );
			if ( IsChineseSeparator ( iCode ) )
				pLastSeparator = pCurBuf;
		}

		return pLastSeparator ? pLastSeparator-pBuffer : iLengthLeft;
	}
};


class CSphRLPResultSplitter : public CSphTokenFilter
{
public:
	CSphRLPResultSplitter ( ISphTokenizer * pTok, const char * szCtxPath )
		: CSphTokenFilter ( pTok )
		, m_bTokenized ( false )
		, m_bNonChineseToken ( false )
		, m_iStart ( 0 )
		, m_iTokenLenBytes ( 0 )
		, m_sCtxPath ( szCtxPath )
	{
		assert ( pTok );

		sphUTF8Encode ( m_pTokenizedMarker, PROXY_TOKENIZED );
		sphUTF8Encode ( m_pNonChineseMarker, PROXY_MORPH );
	}

	virtual void SetBuffer ( const BYTE * szBuffer, int iBufferLength )
	{
		// detect if this is an already tokenized buffer or not
		m_bTokenized = false;
		m_iStart = 0;

		if ( iBufferLength>=PROXY_MARKER_LEN+1 )
		{
			const BYTE * pBufPtr = szBuffer;
			if ( CMP_MARKER ( pBufPtr, m_pTokenizedMarker ) )
			{
				pBufPtr += PROXY_MARKER_LEN+1;
				m_bTokenized = true;
				int iBufSize = iBufferLength-(pBufPtr-szBuffer);
				m_dBuffer.Resize ( iBufSize+1 );
				memcpy ( m_dBuffer.Begin(), pBufPtr, iBufSize );
				m_dBuffer[iBufSize] = '\0';
			}
		}

		if ( !m_bTokenized )
			m_pTokenizer->SetBuffer ( szBuffer, iBufferLength );
	}

	virtual BYTE * GetToken()
	{
		if ( m_bTokenized )
		{
			if ( m_iStart>=m_dBuffer.GetLength() )
				return NULL;

			// check if we have a marker
			if ( m_dBuffer.GetLength()-m_iStart>=PROXY_MARKER_LEN && CMP_MARKER ( (m_dBuffer.Begin()+m_iStart), m_pNonChineseMarker ) )
			{
				m_iStart += PROXY_MARKER_LEN;
				m_bNonChineseToken = true;
			} else
				m_bNonChineseToken = false;

			bool bFound = false;
			for ( int i = m_iStart; i < m_dBuffer.GetLength() && !bFound; i++ )
				if ( m_dBuffer[i]==' ' )
				{
					m_tCurToken.m_szTokenStart = (const char*)&(m_dBuffer[m_iStart]);
					m_dBuffer[i] = 0;
					m_iTokenLenBytes = i-m_iStart;
					m_iStart = i+1;
					bFound = true;
				}

			if ( !bFound )
			{
				m_tCurToken.m_szTokenStart = (const char*)&(m_dBuffer[m_iStart]);
				m_iTokenLenBytes = m_dBuffer.GetLength()-m_iStart;
				m_iStart = m_dBuffer.GetLength();
			}

			// collect extra token data from the text stream
			if ( m_bNonChineseToken )
			{
				int iDataStart = m_iStart;
				while ( m_iStart < m_dBuffer.GetLength() && m_dBuffer[m_iStart]!=' ' )
					m_iStart++;

				DWORD uPacked = strtoul ( (const char*)&(m_dBuffer[iDataStart]), NULL, 16 );
				m_tCurToken.m_bBlended = !!(uPacked & PROXY_BLENDED_FLAG);
				m_tCurToken.m_bBlendedPart = !!(uPacked & PROXY_BLENDED_PART_FLAG);
				m_tCurToken.m_bBoundary = !!(uPacked & PROXY_BOUNDARY_FLAG);
				m_tCurToken.m_bSpecial = !!(uPacked & PROXY_SPECIAL_FLAG);
				m_tCurToken.m_iTokenLen = uPacked & ( ( 1<<6 )-1 );

				// skip space
				if ( m_iStart < m_dBuffer.GetLength() )
				{
					assert ( m_dBuffer[m_iStart]==' ' );
					m_iStart++;
				}

				if ( uPacked & PROXY_HAVE_OVERSHORT )
				{
					iDataStart = m_iStart;
					while ( m_iStart < m_dBuffer.GetLength() && m_dBuffer[m_iStart]!=' ' )
						m_iStart++;

					// skip space
					if ( m_iStart < m_dBuffer.GetLength() )
					{
						assert ( m_dBuffer[m_iStart]==' ' );
						m_dBuffer[m_iStart++] = 0;
					}

					m_tCurToken.m_iOvershortCount = strtoul ( (const char*)&(m_dBuffer[iDataStart]), NULL, 16 );
				} else
					m_tCurToken.m_iOvershortCount = 0;
			}

			m_tCurToken.m_pBufferPtr = (const char*)m_dBuffer.Begin();
			m_tCurToken.m_szTokenEnd = m_tCurToken.m_szTokenStart+m_iTokenLenBytes;

			return (BYTE*)m_tCurToken.m_szTokenStart;
		} else
			return m_pTokenizer->GetToken();
	}

	virtual ISphTokenizer * Clone ( ESphTokenizerClone eMode ) const
	{
		return new CSphRLPResultSplitter ( m_pTokenizer->Clone ( eMode ), m_sCtxPath.cstr() );
	}

	virtual bool GetMorphFlag () const
	{
		if ( !m_bTokenized )
			return true;

		return m_bNonChineseToken;
	}

	virtual int GetLastTokenLen() const
	{
		if ( !m_bTokenized )
			return m_pTokenizer->GetLastTokenLen();

		if ( m_bNonChineseToken )
			return m_tCurToken.m_iTokenLen;

		return sphUTF8Len ( m_tCurToken.m_szTokenStart, m_iTokenLenBytes );
	}

	virtual bool GetBoundary()
	{
		if ( !m_bTokenized )
			return m_pTokenizer->GetBoundary();

		if ( m_bNonChineseToken )
			return m_tCurToken.m_bBoundary;

		return false;
	}

	virtual bool WasTokenSpecial()
	{
		if ( !m_bTokenized )
			return m_pTokenizer->WasTokenSpecial();

		if ( m_bNonChineseToken )
			return m_tCurToken.m_bSpecial;

		return false;
	}

	virtual int	GetOvershortCount ()
	{
		if ( !m_bTokenized )
			return m_pTokenizer->GetOvershortCount();

		if ( m_bNonChineseToken )
			return m_tCurToken.m_iOvershortCount;

		return 0;
	}

	virtual bool TokenIsBlended () const
	{
		if ( !m_bTokenized )
			return m_pTokenizer->TokenIsBlended();

		if ( m_bNonChineseToken )
			return m_tCurToken.m_bBlended;

		return false;
	}

	virtual bool TokenIsBlendedPart () const
	{
		if ( !m_bTokenized )
			return m_pTokenizer->TokenIsBlendedPart();

		if ( m_bNonChineseToken )
			return m_tCurToken.m_bBlendedPart;

		return false;
	}

	virtual const char * GetTokenStart () const
	{
		if ( !m_bTokenized )
			return m_pTokenizer->GetTokenStart();

		return m_tCurToken.m_szTokenStart;
	}

	virtual const char * GetTokenEnd () const
	{
		if ( !m_bTokenized )
			return m_pTokenizer->GetTokenEnd();

		return m_tCurToken.m_szTokenEnd;
	}

	virtual const char * GetBufferPtr () const
	{
		if ( !m_bTokenized )
			return m_pTokenizer->GetBufferPtr();

		return m_tCurToken.m_pBufferPtr;
	}

	virtual const char * GetRLPContext () const
	{
		return m_sCtxPath.cstr();
	}

private:
	bool					m_bTokenized;
	bool					m_bNonChineseToken;
	int						m_iStart;
	StoredToken_t			m_tCurToken;
	int						m_iTokenLenBytes;
	CSphString				m_sCtxPath;
	BYTE					m_pTokenizedMarker[PROXY_MARKER_LEN];
	BYTE					m_pNonChineseMarker[PROXY_MARKER_LEN];
	CSphTightVector<BYTE>	m_dBuffer;
};

#endif

//////////////////////////////////////////////////////////////////////////

ISphTokenizer * sphCreateSBCSTokenizer ()
{
	return new CSphTokenizer_SBCS<false> ();
}


ISphTokenizer * sphCreateUTF8Tokenizer ()
{
	return new CSphTokenizer_UTF8<false> ();
}

ISphTokenizer * sphCreateUTF8ChineseTokenizer ()
{
        return new CSphTokenizer_UTF8Chinese<false> ();
}

ISphTokenizer * sphCreateUTF8NgramTokenizer ()
{
	return new CSphTokenizer_UTF8Ngram<false> ();
}

/////////////////////////////////////////////////////////////////////////////

enum
{
	MASK_CODEPOINT			= 0x00ffffffUL,	// mask off codepoint flags
	MASK_FLAGS				= 0xff000000UL, // mask off codepoint value
	FLAG_CODEPOINT_SPECIAL	= 0x01000000UL,	// this codepoint is special
	FLAG_CODEPOINT_DUAL		= 0x02000000UL,	// this codepoint is special but also a valid word part
	FLAG_CODEPOINT_NGRAM	= 0x04000000UL,	// this codepoint is n-gram indexed
	FLAG_CODEPOINT_SYNONYM	= 0x08000000UL,	// this codepoint is used in synonym tokens only
	FLAG_CODEPOINT_BOUNDARY	= 0x10000000UL,	// this codepoint is phrase boundary
	FLAG_CODEPOINT_IGNORE	= 0x20000000UL,	// this codepoint is ignored
	FLAG_CODEPOINT_BLEND	= 0x40000000UL	// this codepoint is "blended" (indexed both as a character, and as a separator)
};


CSphLowercaser::CSphLowercaser ()
	: m_pData ( NULL )
{
}


void CSphLowercaser::Reset()
{
	SafeDeleteArray ( m_pData );
	m_pData = new int [ CHUNK_SIZE ];
	memset ( m_pData, 0, CHUNK_SIZE*sizeof(int) ); // NOLINT sizeof(int)
	m_iChunks = 1;
	m_pChunk[0] = m_pData; // chunk 0 must always be allocated, for utf-8 tokenizer shortcut to work
	for ( int i=1; i<CHUNK_COUNT; i++ )
		m_pChunk[i] = NULL;
}


CSphLowercaser::~CSphLowercaser ()
{
	SafeDeleteArray ( m_pData );
}


void CSphLowercaser::SetRemap ( const CSphLowercaser * pLC )
{
	if ( !pLC )
		return;

	SafeDeleteArray ( m_pData );

	m_iChunks = pLC->m_iChunks;
	m_pData = new int [ m_iChunks*CHUNK_SIZE ];
	memcpy ( m_pData, pLC->m_pData, sizeof(int)*m_iChunks*CHUNK_SIZE ); // NOLINT sizeof(int)

	for ( int i=0; i<CHUNK_COUNT; i++ )
		m_pChunk[i] = pLC->m_pChunk[i]
			? pLC->m_pChunk[i] - pLC->m_pData + m_pData
			: NULL;
}


void CSphLowercaser::AddRemaps ( const CSphVector<CSphRemapRange> & dRemaps, DWORD uFlags )
{
	if ( !dRemaps.GetLength() )
		return;

	// build new chunks map
	// 0 means "was unused"
	// 1 means "was used"
	// 2 means "is used now"
	int dUsed [ CHUNK_COUNT ];
	for ( int i=0; i<CHUNK_COUNT; i++ )
		dUsed[i] = m_pChunk[i] ? 1 : 0;

	int iNewChunks = m_iChunks;

	ARRAY_FOREACH ( i, dRemaps )
	{
		const CSphRemapRange & tRemap = dRemaps[i];

		#define LOC_CHECK_RANGE(_a) assert ( (_a)>=0 && (_a)<MAX_CODE );
		LOC_CHECK_RANGE ( tRemap.m_iStart );
		LOC_CHECK_RANGE ( tRemap.m_iEnd );
		LOC_CHECK_RANGE ( tRemap.m_iRemapStart );
		LOC_CHECK_RANGE ( tRemap.m_iRemapStart + tRemap.m_iEnd - tRemap.m_iStart );
		#undef LOC_CHECK_RANGE

		for ( int iChunk=( tRemap.m_iStart >> CHUNK_BITS ); iChunk<=( tRemap.m_iEnd >> CHUNK_BITS ); iChunk++ )
			if ( dUsed[iChunk]==0 )
		{
			dUsed[iChunk] = 2;
			iNewChunks++;
		}
	}

	// alloc new tables and copy, if necessary
	if ( iNewChunks>m_iChunks )
	{
		int * pData = new int [ iNewChunks*CHUNK_SIZE ];
		memset ( pData, 0, sizeof(int)*iNewChunks*CHUNK_SIZE ); // NOLINT sizeof(int)

		int * pChunk = pData;
		for ( int i=0; i<CHUNK_COUNT; i++ )
		{
			int * pOldChunk = m_pChunk[i];

			// build new ptr
			if ( dUsed[i] )
			{
				m_pChunk[i] = pChunk;
				pChunk += CHUNK_SIZE;
			}

			// copy old data
			if ( dUsed[i]==1 )
				memcpy ( m_pChunk[i], pOldChunk, sizeof(int)*CHUNK_SIZE ); // NOLINT sizeof(int)
		}
		assert ( pChunk-pData==iNewChunks*CHUNK_SIZE );

		SafeDeleteArray ( m_pData );
		m_pData = pData;
		m_iChunks = iNewChunks;
	}

	// fill new stuff
	ARRAY_FOREACH ( i, dRemaps )
	{
		const CSphRemapRange & tRemap = dRemaps[i];

		DWORD iRemapped = tRemap.m_iRemapStart;
		for ( int j=tRemap.m_iStart; j<=tRemap.m_iEnd; j++, iRemapped++ )
		{
			assert ( m_pChunk [ j >> CHUNK_BITS ] );
			int & iCodepoint = m_pChunk [ j >> CHUNK_BITS ] [ j & CHUNK_MASK ];
			bool bWordPart = ( iCodepoint & MASK_CODEPOINT ) && !( iCodepoint & FLAG_CODEPOINT_SYNONYM );
			int iNew = iRemapped | uFlags | ( iCodepoint & MASK_FLAGS );
			if ( bWordPart && ( uFlags & FLAG_CODEPOINT_SPECIAL ) )
				iNew |= FLAG_CODEPOINT_DUAL;
			iCodepoint = iNew;

			// new code-point flag removes SYNONYM
			if ( ( iCodepoint & FLAG_CODEPOINT_SYNONYM ) && uFlags==0 && iRemapped!=0 )
				iCodepoint &= ~FLAG_CODEPOINT_SYNONYM;
		}
	}
}


void CSphLowercaser::AddSpecials ( const char * sSpecials )
{
	assert ( sSpecials );
	int iSpecials = strlen(sSpecials);

	CSphVector<CSphRemapRange> dRemaps;
	dRemaps.Resize ( iSpecials );
	ARRAY_FOREACH ( i, dRemaps )
		dRemaps[i].m_iStart = dRemaps[i].m_iEnd = dRemaps[i].m_iRemapStart = sSpecials[i];

	AddRemaps ( dRemaps, FLAG_CODEPOINT_SPECIAL );
}

const CSphLowercaser & CSphLowercaser::operator = ( const CSphLowercaser & rhs )
{
	SetRemap ( &rhs );
	return * this;
}

uint64_t CSphLowercaser::GetFNV () const
{
	int iLen = ( sizeof(int) * m_iChunks * CHUNK_SIZE ) / sizeof(BYTE); // NOLINT
	return sphFNV64 ( (BYTE *)m_pData, iLen );
}

int CSphLowercaser::GetMaxCodepointLength () const
{
	int iMax = 0;
	for ( int iChunk=0; iChunk<CHUNK_COUNT; iChunk++ )
	{
		int * pChunk = m_pChunk[iChunk];
		if ( !pChunk )
			continue;

		int * pMax = pChunk + CHUNK_SIZE;
		while ( pChunk<pMax )
		{
			int iCode = *pChunk++ & MASK_CODEPOINT;
			iMax = Max ( iMax, iCode );
		}
	}
	if ( iMax<0x80 )
		return 1;
	if ( iMax<0x800 )
		return 2;
	return 3; // actually, 4 once we hit 0x10000
}

/////////////////////////////////////////////////////////////////////////////

/// parser to build lowercaser from textual config
class CSphCharsetDefinitionParser
{
public:
						CSphCharsetDefinitionParser () : m_bError ( false ) {}
	bool				Parse ( const char * sConfig, CSphVector<CSphRemapRange> & dRanges );
	const char *		GetLastError ();

protected:
	bool				m_bError;
	char				m_sError [ 1024 ];
	const char *		m_pCurrent;

	bool				Error ( const char * sMessage );
	void				SkipSpaces ();
	bool				IsEof ();
	bool				CheckEof ();
	int					HexDigit ( int c );
	int					ParseCharsetCode ();
	bool				AddRange ( const CSphRemapRange & tRange, CSphVector<CSphRemapRange> & dRanges );
};


const char * CSphCharsetDefinitionParser::GetLastError ()
{
	return m_bError ? m_sError : NULL;
}


bool CSphCharsetDefinitionParser::IsEof ()
{
	return ( *m_pCurrent )==0;
}


bool CSphCharsetDefinitionParser::CheckEof ()
{
	if ( IsEof() )
	{
		Error ( "unexpected end of line" );
		return true;
	} else
	{
		return false;
	}
}


bool CSphCharsetDefinitionParser::Error ( const char * sMessage )
{
	char sErrorBuffer[32];
	strncpy ( sErrorBuffer, m_pCurrent, sizeof(sErrorBuffer) );
	sErrorBuffer [ sizeof(sErrorBuffer)-1 ] = '\0';

	snprintf ( m_sError, sizeof(m_sError), "%s near '%s'",
		sMessage, sErrorBuffer );
	m_sError [ sizeof(m_sError)-1 ] = '\0';

	m_bError = true;
	return false;
}


int CSphCharsetDefinitionParser::HexDigit ( int c )
{
	if ( c>='0' && c<='9' ) return c-'0';
	if ( c>='a' && c<='f' ) return c-'a'+10;
	if ( c>='A' && c<='F' ) return c-'A'+10;
	return 0;
}


void CSphCharsetDefinitionParser::SkipSpaces ()
{
	while ( ( *m_pCurrent ) && isspace ( (BYTE)*m_pCurrent ) )
		m_pCurrent++;
}


int CSphCharsetDefinitionParser::ParseCharsetCode ()
{
	const char * p = m_pCurrent;
	int iCode = 0;

	if ( p[0]=='U' && p[1]=='+' )
	{
		p += 2;
		while ( isxdigit(*p) )
		{
			iCode = iCode*16 + HexDigit ( *p++ );
		}
		while ( isspace(*p) )
			p++;

	} else
	{
		if ( (*(BYTE*)p)<32 || (*(BYTE*)p)>127 )
		{
			Error ( "non-ASCII characters not allowed, use 'U+00AB' syntax" );
			return -1;
		}

		iCode = *p++;
		while ( isspace(*p) )
			p++;
	}

	m_pCurrent = p;
	return iCode;
}

bool CSphCharsetDefinitionParser::AddRange ( const CSphRemapRange & tRange, CSphVector<CSphRemapRange> & dRanges )
{
	if ( tRange.m_iRemapStart>=0x20 )
	{
		dRanges.Add ( tRange );
		return true;
	}

	CSphString sError;
	sError.SetSprintf ( "dest range (U+%x) below U+20, not allowed", tRange.m_iRemapStart );
	Error ( sError.cstr() );
	return false;
}


bool CSphCharsetDefinitionParser::Parse ( const char * sConfig, CSphVector<CSphRemapRange> & dRanges )
{
	m_pCurrent = sConfig;
	dRanges.Reset ();

	// do parse
	while ( *m_pCurrent )
	{
		SkipSpaces ();
		if ( IsEof () )
			break;

		// check for stray comma
		if ( *m_pCurrent==',' )
			return Error ( "stray ',' not allowed, use 'U+002C' instead" );

		// parse char code
		const char * pStart = m_pCurrent;
		int iStart = ParseCharsetCode();
		if ( iStart<0 )
			return false;

		// stray char?
		if ( !*m_pCurrent || *m_pCurrent==',' )
		{
			// stray char
			if ( !AddRange ( CSphRemapRange ( iStart, iStart, iStart ), dRanges ) )
				return false;

			if ( IsEof () )
				break;
			m_pCurrent++;
			continue;
		}

		// stray remap?
		if ( m_pCurrent[0]=='-' && m_pCurrent[1]=='>' )
		{
			// parse and add
			m_pCurrent += 2;
			int iDest = ParseCharsetCode ();
			if ( iDest<0 )
				return false;
			if ( !AddRange ( CSphRemapRange ( iStart, iStart, iDest ), dRanges ) )
				return false;

			// it's either end of line now, or must be followed by comma
			if ( *m_pCurrent )
				if ( *m_pCurrent++!=',' )
					return Error ( "syntax error" );
			continue;
		}

		// range start?
		if (!( m_pCurrent[0]=='.' && m_pCurrent[1]=='.' ))
			return Error ( "syntax error" );
		m_pCurrent += 2;

		SkipSpaces ();
		if ( CheckEof () )
			return false;

		// parse range end char code
		int iEnd = ParseCharsetCode ();
		if ( iEnd<0 )
			return false;
		if ( iStart>iEnd )
		{
			m_pCurrent = pStart;
			return Error ( "range end less than range start" );
		}

		// stray range?
		if ( !*m_pCurrent || *m_pCurrent==',' )
		{
			if ( !AddRange ( CSphRemapRange ( iStart, iEnd, iStart ), dRanges ) )
				return false;

			if ( IsEof () )
				break;
			m_pCurrent++;
			continue;
		}

		// "checkerboard" range?
		if ( m_pCurrent[0]=='/' && m_pCurrent[1]=='2' )
		{
			for ( int i=iStart; i<iEnd; i+=2 )
			{
				if ( !AddRange ( CSphRemapRange ( i, i, i+1 ), dRanges ) )
					return false;
				if ( !AddRange ( CSphRemapRange ( i+1, i+1, i+1 ), dRanges ) )
					return false;
			}

			// skip "/2", expect ","
			m_pCurrent += 2;
			SkipSpaces ();
			if ( *m_pCurrent )
				if ( *m_pCurrent++!=',' )
					return Error ( "expected end of line or ','" );
			continue;
		}

		// remapped range?
		if (!( m_pCurrent[0]=='-' && m_pCurrent[1]=='>' ))
			return Error ( "expected end of line, ',' or '-><char>'" );
		m_pCurrent += 2;

		SkipSpaces ();
		if ( CheckEof () )
			return false;

		// parse dest start
		const char * pRemapStart = m_pCurrent;
		int iRemapStart = ParseCharsetCode ();
		if ( iRemapStart<0 )
			return false;

		// expect '..'
		if ( CheckEof () )
			return false;
		if (!( m_pCurrent[0]=='.' && m_pCurrent[1]=='.' ))
			return Error ( "expected '..'" );
		m_pCurrent += 2;

		// parse dest end
		int iRemapEnd = ParseCharsetCode ();
		if ( iRemapEnd<0 )
			return false;

		// check dest range
		if ( iRemapStart>iRemapEnd )
		{
			m_pCurrent = pRemapStart;
			return Error ( "dest range end less than dest range start" );
		}

		// check for length mismatch
		if ( ( iRemapEnd-iRemapStart )!=( iEnd-iStart ) )
		{
			m_pCurrent = pStart;
			return Error ( "dest range length must match src range length" );
		}

		// remapped ok
		if ( !AddRange ( CSphRemapRange ( iStart, iEnd, iRemapStart ), dRanges ) )
			return false;

		if ( IsEof () )
			break;
		if ( *m_pCurrent!=',' )
			return Error ( "expected ','" );
		m_pCurrent++;
	}

	dRanges.Sort ();
	for ( int i=0; i<dRanges.GetLength()-1; i++ )
	{
		if ( dRanges[i].m_iEnd>=dRanges[i+1].m_iStart )
		{
			// FIXME! add an ambiguity check
			dRanges[i].m_iEnd = Max ( dRanges[i].m_iEnd, dRanges[i+1].m_iEnd );
			dRanges.Remove ( i+1 );
			i--;
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////

bool sphParseCharset ( const char * sCharset, CSphVector<CSphRemapRange> & dRemaps )
{
	CSphCharsetDefinitionParser tParser;
	return tParser.Parse ( sCharset, dRemaps );
}

/////////////////////////////////////////////////////////////////////////////

CSphSavedFile::CSphSavedFile ()
	: m_uSize	( 0 )
	, m_uCTime	( 0 )
	, m_uMTime	( 0 )
	, m_uCRC32	( 0 )
{
}


CSphEmbeddedFiles::CSphEmbeddedFiles ()
	: m_bEmbeddedSynonyms	( false )
	, m_bEmbeddedStopwords	( false )
	, m_bEmbeddedWordforms	( false )
{
}


void CSphEmbeddedFiles::Reset()
{
	m_dSynonyms.Reset();
	m_dStopwordFiles.Reset();
	m_dStopwords.Reset();
	m_dWordforms.Reset();
	m_dWordformFiles.Reset();
}


CSphTokenizerSettings::CSphTokenizerSettings ()
	: m_iType				( TOKENIZER_UTF8 )
	, m_iMinWordLen			( 1 )
	, m_iNgramLen			( 0 )
{
}


void LoadTokenizerSettings ( CSphReader & tReader, CSphTokenizerSettings & tSettings,
	CSphEmbeddedFiles & tEmbeddedFiles, DWORD uVersion, CSphString & sWarning )
{
	if ( uVersion<9 )
		return;

	tSettings.m_iType = tReader.GetByte ();
	tSettings.m_sCaseFolding = tReader.GetString ();
	tSettings.m_iMinWordLen = tReader.GetDword ();
	tEmbeddedFiles.m_bEmbeddedSynonyms = false;
	if ( uVersion>=30 )
	{
		tEmbeddedFiles.m_bEmbeddedSynonyms = !!tReader.GetByte();
		if ( tEmbeddedFiles.m_bEmbeddedSynonyms )
		{
			int nSynonyms = (int)tReader.GetDword();
			tEmbeddedFiles.m_dSynonyms.Resize ( nSynonyms );
			ARRAY_FOREACH ( i, tEmbeddedFiles.m_dSynonyms )
				tEmbeddedFiles.m_dSynonyms[i] = tReader.GetString();
		}
	}

	tSettings.m_sSynonymsFile = tReader.GetString ();
	ReadFileInfo ( tReader, tSettings.m_sSynonymsFile.cstr (),
		tEmbeddedFiles.m_tSynonymFile, tEmbeddedFiles.m_bEmbeddedSynonyms ? NULL : &sWarning );
	tSettings.m_sBoundary = tReader.GetString ();
	tSettings.m_sIgnoreChars = tReader.GetString ();
	tSettings.m_iNgramLen = tReader.GetDword ();
	tSettings.m_sNgramChars = tReader.GetString ();
	if ( uVersion>=15 )
		tSettings.m_sBlendChars = tReader.GetString ();
	if ( uVersion>=24 )
		tSettings.m_sBlendMode = tReader.GetString();
        tSettings.m_sChineseDictionary = tReader.GetString();
}


/// gets called from and MUST be in sync with RtIndex_t::SaveDiskHeader()!
/// note that SaveDiskHeader() occasionaly uses some PREVIOUS format version!
void SaveTokenizerSettings ( CSphWriter & tWriter, ISphTokenizer * pTokenizer, int iEmbeddedLimit )
{
	assert ( pTokenizer );

	const CSphTokenizerSettings & tSettings = pTokenizer->GetSettings ();
	tWriter.PutByte ( tSettings.m_iType );
	tWriter.PutString ( tSettings.m_sCaseFolding.cstr () );
	tWriter.PutDword ( tSettings.m_iMinWordLen );

	bool bEmbedSynonyms = pTokenizer->GetSynFileInfo ().m_uSize<=(SphOffset_t)iEmbeddedLimit;
	tWriter.PutByte ( bEmbedSynonyms ? 1 : 0 );
	if ( bEmbedSynonyms )
		pTokenizer->WriteSynonyms ( tWriter );

	tWriter.PutString ( tSettings.m_sSynonymsFile.cstr () );
	WriteFileInfo ( tWriter, pTokenizer->GetSynFileInfo () );
	tWriter.PutString ( tSettings.m_sBoundary.cstr () );
	tWriter.PutString ( tSettings.m_sIgnoreChars.cstr () );
	tWriter.PutDword ( tSettings.m_iNgramLen );
	tWriter.PutString ( tSettings.m_sNgramChars.cstr () );
	tWriter.PutString ( tSettings.m_sBlendChars.cstr () );
	tWriter.PutString ( tSettings.m_sBlendMode.cstr () );
        tWriter.PutString ( tSettings.m_sChineseDictionary.cstr () );
}


void LoadDictionarySettings ( CSphReader & tReader, CSphDictSettings & tSettings,
	CSphEmbeddedFiles & tEmbeddedFiles, DWORD uVersion, CSphString & sWarning )
{
	if ( uVersion<9 )
		return;

	tSettings.m_sMorphology = tReader.GetString ();

	tEmbeddedFiles.m_bEmbeddedStopwords = false;
	if ( uVersion>=30 )
	{
		tEmbeddedFiles.m_bEmbeddedStopwords = !!tReader.GetByte();
		if ( tEmbeddedFiles.m_bEmbeddedStopwords )
		{
			int nStopwords = (int)tReader.GetDword();
			tEmbeddedFiles.m_dStopwords.Resize ( nStopwords );
			ARRAY_FOREACH ( i, tEmbeddedFiles.m_dStopwords )
				tEmbeddedFiles.m_dStopwords[i] = (SphWordID_t)tReader.UnzipOffset();
		}
	}

	tSettings.m_sStopwords = tReader.GetString ();
	int nFiles = tReader.GetDword ();

	CSphString sFile;
	tEmbeddedFiles.m_dStopwordFiles.Resize ( nFiles );
	for ( int i = 0; i < nFiles; i++ )
	{
		sFile = tReader.GetString ();
		ReadFileInfo ( tReader, sFile.cstr (), tEmbeddedFiles.m_dStopwordFiles[i], tEmbeddedFiles.m_bEmbeddedSynonyms ? NULL : &sWarning );
	}

	tEmbeddedFiles.m_bEmbeddedWordforms = false;
	if ( uVersion>=30 )
	{
		tEmbeddedFiles.m_bEmbeddedWordforms = !!tReader.GetByte();
		if ( tEmbeddedFiles.m_bEmbeddedWordforms )
		{
			int nWordforms = (int)tReader.GetDword();
			tEmbeddedFiles.m_dWordforms.Resize ( nWordforms );
			ARRAY_FOREACH ( i, tEmbeddedFiles.m_dWordforms )
				tEmbeddedFiles.m_dWordforms[i] = tReader.GetString();
		}
	}

	if ( uVersion>=29 )
		tSettings.m_dWordforms.Resize ( tReader.GetDword() );
	else
		tSettings.m_dWordforms.Resize(1);

	tEmbeddedFiles.m_dWordformFiles.Resize ( tSettings.m_dWordforms.GetLength() );
	ARRAY_FOREACH ( i, tSettings.m_dWordforms )
	{
		tSettings.m_dWordforms[i] = tReader.GetString();
		ReadFileInfo ( tReader, tSettings.m_dWordforms[i].cstr(),
			tEmbeddedFiles.m_dWordformFiles[i], tEmbeddedFiles.m_bEmbeddedWordforms ? NULL : &sWarning );
	}

	if ( uVersion>=13 )
		tSettings.m_iMinStemmingLen = tReader.GetDword ();

	tSettings.m_bWordDict = false; // default to crc for old indexes
	if ( uVersion>=21 )
	{
		tSettings.m_bWordDict = ( tReader.GetByte()!=0 );
		if ( !tSettings.m_bWordDict )
			sphWarning ( "dict=crc deprecated, use dict=keywords instead" );
	}

	if ( uVersion>=36 )
		tSettings.m_bStopwordsUnstemmed = ( tReader.GetByte()!=0 );

	if ( uVersion>=37 )
		tSettings.m_sMorphFingerprint = tReader.GetString();
}


/// gets called from and MUST be in sync with RtIndex_t::SaveDiskHeader()!
/// note that SaveDiskHeader() occasionaly uses some PREVIOUS format version!
void SaveDictionarySettings ( CSphWriter & tWriter, CSphDict * pDict, bool bForceWordDict, int iEmbeddedLimit )
{
	assert ( pDict );
	const CSphDictSettings & tSettings = pDict->GetSettings ();

	tWriter.PutString ( tSettings.m_sMorphology.cstr () );
	const CSphVector <CSphSavedFile> & dSWFileInfos = pDict->GetStopwordsFileInfos ();
	SphOffset_t uTotalSize = 0;
	ARRAY_FOREACH ( i, dSWFileInfos )
		uTotalSize += dSWFileInfos[i].m_uSize;

	bool bEmbedStopwords = uTotalSize<=(SphOffset_t)iEmbeddedLimit;
	tWriter.PutByte ( bEmbedStopwords ? 1 : 0 );
	if ( bEmbedStopwords )
		pDict->WriteStopwords ( tWriter );

	tWriter.PutString ( tSettings.m_sStopwords.cstr () );
	tWriter.PutDword ( dSWFileInfos.GetLength () );
	ARRAY_FOREACH ( i, dSWFileInfos )
	{
		tWriter.PutString ( dSWFileInfos[i].m_sFilename.cstr () );
		WriteFileInfo ( tWriter, dSWFileInfos[i] );
	}

	const CSphVector <CSphSavedFile> & dWFFileInfos = pDict->GetWordformsFileInfos ();
	uTotalSize = 0;
	ARRAY_FOREACH ( i, dWFFileInfos )
		uTotalSize += dWFFileInfos[i].m_uSize;

	bool bEmbedWordforms = uTotalSize<=(SphOffset_t)iEmbeddedLimit;
	tWriter.PutByte ( bEmbedWordforms ? 1 : 0 );
	if ( bEmbedWordforms )
		pDict->WriteWordforms ( tWriter );

	tWriter.PutDword ( dWFFileInfos.GetLength() );
	ARRAY_FOREACH ( i, dWFFileInfos )
	{
		tWriter.PutString ( dWFFileInfos[i].m_sFilename.cstr() );
		WriteFileInfo ( tWriter, dWFFileInfos[i] );
	}

	tWriter.PutDword ( tSettings.m_iMinStemmingLen );
	tWriter.PutByte ( tSettings.m_bWordDict || bForceWordDict );
	tWriter.PutByte ( tSettings.m_bStopwordsUnstemmed );
	tWriter.PutString ( pDict->GetMorphDataFingerprint() );
}


void LoadFieldFilterSettings ( CSphReader & tReader, CSphFieldFilterSettings & tFieldFilterSettings )
{
	int nRegexps = tReader.GetDword();
	if ( !nRegexps )
		return;

	tFieldFilterSettings.m_dRegexps.Resize ( nRegexps );
	ARRAY_FOREACH ( i, tFieldFilterSettings.m_dRegexps )
		tFieldFilterSettings.m_dRegexps[i] = tReader.GetString();

	tFieldFilterSettings.m_bUTF8 = !!tReader.GetByte();
}


void SaveFieldFilterSettings ( CSphWriter & tWriter, ISphFieldFilter * pFieldFilter )
{
	if ( !pFieldFilter )
	{
		tWriter.PutDword ( 0 );
		return;
	}

	CSphFieldFilterSettings tSettings;
	pFieldFilter->GetSettings ( tSettings );

	tWriter.PutDword ( tSettings.m_dRegexps.GetLength() );
	ARRAY_FOREACH ( i, tSettings.m_dRegexps )
		tWriter.PutString ( tSettings.m_dRegexps[i] );

	tWriter.PutByte ( tSettings.m_bUTF8 ? 1 : 0 );
}


static inline bool ShortTokenFilter ( BYTE * pToken, int iLen )
{
	return pToken[0]=='*' || ( iLen > 0 && pToken[iLen-1]=='*' );
}

/////////////////////////////////////////////////////////////////////////////

ISphTokenizer::ISphTokenizer ()
	: m_iLastTokenLen ( 0 )
	, m_bTokenBoundary ( false )
	, m_bBoundary ( false )
	, m_bWasSpecial ( false )
	, m_bWasSynonym ( false )
	, m_bEscaped ( false )
	, m_iOvershortCount ( 0 )
	, m_eTokenMorph ( SPH_TOKEN_MORPH_RAW )
	, m_bBlended ( false )
	, m_bNonBlended ( true )
	, m_bBlendedPart ( false )
	, m_bBlendAdd ( false )
	, m_uBlendVariants ( BLEND_TRIM_NONE )
	, m_uBlendVariantsPending ( 0 )
	, m_bBlendSkipPure ( false )
	, m_bShortTokenFilter ( false )
	, m_bDetectSentences ( false )
	, m_bPhrase ( false )
{}


bool ISphTokenizer::SetCaseFolding ( const char * sConfig, CSphString & sError )
{
	CSphVector<CSphRemapRange> dRemaps;
	CSphCharsetDefinitionParser tParser;
	if ( !tParser.Parse ( sConfig, dRemaps ) )
	{
		sError = tParser.GetLastError();
		return false;
	}

	const int MIN_CODE = 0x21;
	ARRAY_FOREACH ( i, dRemaps )
	{
		CSphRemapRange & tMap = dRemaps[i];

		if ( tMap.m_iStart<MIN_CODE || tMap.m_iStart>=m_tLC.MAX_CODE )
		{
			sphWarning ( "wrong character mapping start specified: U+%x, should be between U+%x and U+%x (inclusive); CLAMPED",
				tMap.m_iStart, MIN_CODE, m_tLC.MAX_CODE-1 );
			tMap.m_iStart = Min ( Max ( tMap.m_iStart, MIN_CODE ), m_tLC.MAX_CODE-1 );
		}

		if ( tMap.m_iEnd<MIN_CODE || tMap.m_iEnd>=m_tLC.MAX_CODE )
		{
			sphWarning ( "wrong character mapping end specified: U+%x, should be between U+%x and U+%x (inclusive); CLAMPED",
				tMap.m_iEnd, MIN_CODE, m_tLC.MAX_CODE-1 );
			tMap.m_iEnd = Min ( Max ( tMap.m_iEnd, MIN_CODE ), m_tLC.MAX_CODE-1 );
		}

		if ( tMap.m_iRemapStart<MIN_CODE || tMap.m_iRemapStart>=m_tLC.MAX_CODE )
		{
			sphWarning ( "wrong character remapping start specified: U+%x, should be between U+%x and U+%x (inclusive); CLAMPED",
				tMap.m_iRemapStart, MIN_CODE, m_tLC.MAX_CODE-1 );
			tMap.m_iRemapStart = Min ( Max ( tMap.m_iRemapStart, MIN_CODE ), m_tLC.MAX_CODE-1 );
		}

		int iRemapEnd = tMap.m_iRemapStart+tMap.m_iEnd-tMap.m_iStart;
		if ( iRemapEnd<MIN_CODE || iRemapEnd>=m_tLC.MAX_CODE )
		{
			sphWarning ( "wrong character remapping end specified: U+%x, should be between U+%x and U+%x (inclusive); IGNORED",
				iRemapEnd, MIN_CODE, m_tLC.MAX_CODE-1 );
			dRemaps.Remove(i);
			i--;
		}
	}

	m_tLC.Reset ();
	m_tLC.AddRemaps ( dRemaps, 0 );
	return true;
}


void ISphTokenizer::AddPlainChar ( char c )
{
	CSphVector<CSphRemapRange> dTmp ( 1 );
	dTmp[0].m_iStart = dTmp[0].m_iEnd = dTmp[0].m_iRemapStart = c;
	m_tLC.AddRemaps ( dTmp, 0 );
}


void ISphTokenizer::AddSpecials ( const char * sSpecials )
{
	m_tLC.AddSpecials ( sSpecials );
}


static int TokenizeOnWhitespace ( CSphVector<CSphString> & dTokens, const BYTE * sFrom, bool bUtf8 )
{
	BYTE sAccum [ 3*SPH_MAX_WORD_LEN+16 ];
	BYTE * pAccum = sAccum;
	int iAccum = 0;

	for ( ;; )
	{
		int iCode = bUtf8 ? sphUTF8Decode(sFrom) : *sFrom++;

		// eof or whitespace?
		if ( !iCode || sphIsSpace(iCode) )
		{
			// flush accum
			if ( iAccum )
			{
				*pAccum = '\0';
				dTokens.Add ( (char*)sAccum );

				pAccum = sAccum;
				iAccum = 0;
			}

			// break on eof
			if ( !iCode )
				break;
		} else
		{
			// accumulate everything else
			if ( iAccum<SPH_MAX_WORD_LEN )
			{
				if ( bUtf8 && ( pAccum-sAccum+SPH_MAX_UTF8_BYTES<=(int)sizeof(sAccum) ) )
				{
					pAccum += sphUTF8Encode ( pAccum, iCode );
					iAccum++;
				} else
				{
					*pAccum++ = BYTE(iCode);
					iAccum++;
				}
			}
		}
	}

	return dTokens.GetLength();
}


static BYTE * sphTrim ( BYTE * s )
{
	// skip to first non-whitespace from start
	while ( *s && sphIsSpace(*s) )
		s++;
	if ( !*s )
		return s;

	// find the end
	BYTE * sEnd = s;
	while ( *sEnd )
		sEnd++;
	sEnd--;

	// skip to first non-whitespace from end
	while ( sEnd>s && sphIsSpace(*sEnd) )
		sEnd--;

	*++sEnd = '\0';
	return s;
}


void ISphTokenizer::Setup ( const CSphTokenizerSettings & tSettings )
{
	m_tSettings = tSettings;
}


ISphTokenizer * ISphTokenizer::Create ( const CSphTokenizerSettings & tSettings, const CSphEmbeddedFiles * pFiles, CSphString & sError )
{
	CSphScopedPtr<ISphTokenizer> pTokenizer ( NULL );

	switch ( tSettings.m_iType )
	{
		case TOKENIZER_SBCS:	pTokenizer = sphCreateSBCSTokenizer (); break;
		case TOKENIZER_UTF8:	pTokenizer = sphCreateUTF8Tokenizer (); break;
                case TOKENIZER_CHINESE: pTokenizer = sphCreateUTF8ChineseTokenizer (); break;
		case TOKENIZER_NGRAM:	pTokenizer = sphCreateUTF8NgramTokenizer (); break;
		default:
			sError.SetSprintf ( "failed to create tokenizer (unknown charset type '%d')", tSettings.m_iType );
			return NULL;
	}

	pTokenizer->Setup ( tSettings );

	if ( !tSettings.m_sCaseFolding.IsEmpty () && !pTokenizer->SetCaseFolding ( tSettings.m_sCaseFolding.cstr (), sError ) )
	{
		sError.SetSprintf ( "'charset_table': %s", sError.cstr() );
		return NULL;
	}

	if ( !tSettings.m_sSynonymsFile.IsEmpty () && !pTokenizer->LoadSynonyms ( tSettings.m_sSynonymsFile.cstr (),
		pFiles && pFiles->m_bEmbeddedSynonyms ? pFiles : NULL, sError ) )
	{
		sError.SetSprintf ( "'synonyms': %s", sError.cstr() );
		return NULL;
	}

	if ( !tSettings.m_sBoundary.IsEmpty () && !pTokenizer->SetBoundary ( tSettings.m_sBoundary.cstr (), sError ) )
	{
		sError.SetSprintf ( "'phrase_boundary': %s", sError.cstr() );
		return NULL;
	}

	if ( !tSettings.m_sIgnoreChars.IsEmpty () && !pTokenizer->SetIgnoreChars ( tSettings.m_sIgnoreChars.cstr (), sError ) )
	{
		sError.SetSprintf ( "'ignore_chars': %s", sError.cstr() );
		return NULL;
	}

	if ( !tSettings.m_sBlendChars.IsEmpty () && !pTokenizer->SetBlendChars ( tSettings.m_sBlendChars.cstr (), sError ) )
	{
		sError.SetSprintf ( "'blend_chars': %s", sError.cstr() );
		return NULL;
	}

	if ( !pTokenizer->SetBlendMode ( tSettings.m_sBlendMode.cstr (), sError ) )
	{
		sError.SetSprintf ( "'blend_mode': %s", sError.cstr() );
		return NULL;
	}
	
        if ( !tSettings.m_sChineseDictionary.IsEmpty () && !pTokenizer->SetChineseDictionary ( tSettings.m_sChineseDictionary.cstr (), sError ) )
        {
                sError.SetSprintf ( "'chinese_dictionary': %s", sError.cstr() );
                return NULL;
        }
        
	pTokenizer->SetNgramLen ( tSettings.m_iNgramLen );

	if ( !tSettings.m_sNgramChars.IsEmpty () && !pTokenizer->SetNgramChars ( tSettings.m_sNgramChars.cstr (), sError ) )
	{
		sError.SetSprintf ( "'ngram_chars': %s", sError.cstr() );
		return NULL;
	}

	return pTokenizer.LeakPtr ();
}


ISphTokenizer * ISphTokenizer::CreateMultiformFilter ( ISphTokenizer * pTokenizer, const CSphMultiformContainer * pContainer )
{
	if ( !pContainer )
		return pTokenizer;
	return new CSphMultiformTokenizer ( pTokenizer, pContainer );
}


ISphTokenizer * ISphTokenizer::CreateBigramFilter ( ISphTokenizer * pTokenizer, ESphBigram eBigramIndex, const CSphString & sBigramWords, CSphString & sError )
{
	assert ( pTokenizer );

	if ( eBigramIndex==SPH_BIGRAM_NONE )
		return pTokenizer;

	CSphVector<CSphString> dFreq;
	if ( eBigramIndex!=SPH_BIGRAM_ALL )
	{
		const BYTE * pTok = NULL;
		pTokenizer->SetBuffer ( (const BYTE*)sBigramWords.cstr(), sBigramWords.Length() );
		while ( ( pTok = pTokenizer->GetToken() )!=NULL )
			dFreq.Add ( (const char*)pTok );

		if ( !dFreq.GetLength() )
		{
			SafeDelete ( pTokenizer );
			sError.SetSprintf ( "bigram_freq_words does not contain any valid words" );
			return NULL;
		}
	}

	return new CSphBigramTokenizer ( pTokenizer, eBigramIndex, dFreq );
}


#if USE_RLP
ISphTokenizer * ISphTokenizer::CreateRLPFilter ( ISphTokenizer * pTokenizer, bool bChineseRLP, const char * szRLPRoot,
												const char * szRLPEnv, const char * szRLPCtx, bool bFilterChinese, CSphString & sError )
{
	assert ( pTokenizer );
	if ( !bChineseRLP )
		return pTokenizer;

	if ( !pTokenizer->IsUtf8() )
	{
		sError.SetSprintf ( "both rlp_chinese and rlp_chinese_batched require UTF-8" );
		SafeDelete ( pTokenizer );
		return NULL;
	}

	CSphRLPTokenizer * pRLP = new CSphRLPTokenizer ( pTokenizer, szRLPRoot, szRLPEnv, szRLPCtx, bFilterChinese );
	if ( !pRLP->Init ( sError ) )
		SafeDelete ( pRLP );

	return pRLP;
}


ISphTokenizer * ISphTokenizer::CreateRLPResultSplitter ( ISphTokenizer * pTokenizer, const char * szRLPCtx )
{
	assert ( pTokenizer );
	return new CSphRLPResultSplitter ( pTokenizer, szRLPCtx );
}

#endif

bool ISphTokenizer::AddSpecialsSPZ ( const char * sSpecials, const char * sDirective, CSphString & sError )
{
	for ( int i=0; sSpecials[i]; i++ )
	{
		int iCode = m_tLC.ToLower ( sSpecials[i] );
		if ( iCode & ( FLAG_CODEPOINT_NGRAM | FLAG_CODEPOINT_BOUNDARY | FLAG_CODEPOINT_IGNORE ) )
		{
			sError.SetSprintf ( "%s requires that character '%c' is not in ngram_chars, phrase_boundary, or ignore_chars",
				sDirective, sSpecials[i] );
			return false;
		}
	}

	AddSpecials ( sSpecials );
	return true;
}


bool ISphTokenizer::EnableSentenceIndexing ( CSphString & sError )
{
	const char sSpecials[] = { '.', '?', '!', MAGIC_CODE_PARAGRAPH, 0 };

	if ( !AddSpecialsSPZ ( sSpecials, "index_sp", sError ) )
		return false;

	m_bDetectSentences = true;
	return true;
}


bool ISphTokenizer::EnableZoneIndexing ( CSphString & sError )
{
	static const char sSpecials[] = { MAGIC_CODE_ZONE, 0 };
	return AddSpecialsSPZ ( sSpecials, "index_zones", sError );
}

uint64_t ISphTokenizer::GetSettingsFNV () const
{
	uint64_t uHash = m_tLC.GetFNV();

	DWORD uFlags = 0;
	if ( m_bBlendSkipPure )
		uFlags |= 1<<1;
	if ( m_bShortTokenFilter )
		uFlags |= 1<<2;
	uHash = sphFNV64 ( (const BYTE *)&uFlags, sizeof(uFlags), uHash );
	uHash = sphFNV64 ( (const BYTE *)&m_uBlendVariants, sizeof(m_uBlendVariants), uHash );

	uHash = sphFNV64 ( (const BYTE *)&m_tSettings.m_iType, sizeof(m_tSettings.m_iType), uHash );
	uHash = sphFNV64 ( (const BYTE *)&m_tSettings.m_iMinWordLen, sizeof(m_tSettings.m_iMinWordLen), uHash );
	uHash = sphFNV64 ( (const BYTE *)&m_tSettings.m_iNgramLen, sizeof(m_tSettings.m_iNgramLen), uHash );

	return uHash;
}

//////////////////////////////////////////////////////////////////////////

CSphTokenizerBase::CSphTokenizerBase ()
	: m_pBuffer		( NULL )
	, m_pBufferMax	( NULL )
	, m_pCur		( NULL )
	, m_pTokenStart ( NULL )
	, m_pTokenEnd	( NULL )
	, m_iAccum		( 0 )
	, m_bHasBlend	( false )
	, m_pBlendStart		( NULL )
	, m_pBlendEnd		( NULL )
	, m_eMode ( SPH_CLONE_INDEX )
{
	m_pAccum = m_sAccum;
}


bool CSphTokenizerBase::SetCaseFolding ( const char * sConfig, CSphString & sError )
{
	assert ( m_eMode!=SPH_CLONE_QUERY_LIGHTWEIGHT );
	if ( m_dSynonyms.GetLength() )
	{
		sError = "SetCaseFolding() must not be called after LoadSynonyms()";
		return false;
	}
	m_bHasBlend = false;
	return ISphTokenizer::SetCaseFolding ( sConfig, sError );
}


bool CSphTokenizerBase::SetBlendChars ( const char * sConfig, CSphString & sError )
{
	assert ( m_eMode!=SPH_CLONE_QUERY_LIGHTWEIGHT );
	m_bHasBlend = ISphTokenizer::SetBlendChars ( sConfig, sError );
	return m_bHasBlend;
}


bool CSphTokenizerBase::LoadSynonym ( char * sBuffer, const char * sFilename,
	int iLine, CSphSynonymHash & tHash, CSphString & sError )
{
	assert ( m_eMode!=SPH_CLONE_QUERY_LIGHTWEIGHT );
	CSphVector<CSphString> dFrom;

	// extract map-from and map-to parts
	char * sSplit = strstr ( sBuffer, "=>" );
	if ( !sSplit )
	{
		sError.SetSprintf ( "%s line %d: mapping token (=>) not found", sFilename, iLine );
		return false;
	}

	const BYTE * sFrom = (BYTE *) sBuffer;
	BYTE * sTo = (BYTE *)( sSplit + strlen ( "=>" ) );
	*sSplit = '\0';

	// tokenize map-from
	if ( !TokenizeOnWhitespace ( dFrom, sFrom, IsUtf8() ) )
	{
		sError.SetSprintf ( "%s line %d: empty map-from part", sFilename, iLine );
		return false;
	}

	// trim map-to
	sTo = sphTrim ( sTo );
	if ( !*sTo )
	{
		sError.SetSprintf ( "%s line %d: empty map-to part", sFilename, iLine );
		return false;
	}

	// check lengths
	ARRAY_FOREACH ( i, dFrom )
	{
		int iFromLen = IsUtf8() ? sphUTF8Len ( dFrom[i].cstr() ) : strlen ( dFrom[i].cstr() );
		if ( iFromLen>SPH_MAX_WORD_LEN )
		{
			sError.SetSprintf ( "%s line %d: map-from token too long (over %d bytes)", sFilename, iLine, SPH_MAX_WORD_LEN );
			return false;
		}
	}

	int iToLen = IsUtf8() ? sphUTF8Len ( (const char*)sTo ) : strlen ( (const char*)sTo );
	if ( iToLen>SPH_MAX_WORD_LEN )
	{
		sError.SetSprintf ( "%s line %d: map-to token too long (over %d bytes)", sFilename, iLine, SPH_MAX_WORD_LEN );
		return false;
	}

	// pack and store it
	int iFromLen = 1;
	ARRAY_FOREACH ( i, dFrom )
		iFromLen += strlen ( dFrom[i].cstr() ) + 1;

	if ( iFromLen>MAX_SYNONYM_LEN )
	{
		sError.SetSprintf ( "%s line %d: map-from part too long (over %d bytes)", sFilename, iLine, MAX_SYNONYM_LEN );
		return false;
	}

	CSphSynonym & tSyn = m_dSynonyms.Add ();
	tSyn.m_sFrom.Reserve ( iFromLen );
	tSyn.m_iFromLen = iFromLen;
	tSyn.m_sTo = (char*)sTo;
	tSyn.m_iToLen = iToLen;

	char * sCur = const_cast<char*> ( tSyn.m_sFrom.cstr() );
	ARRAY_FOREACH ( i, dFrom )
	{
		int iLen = strlen ( dFrom[i].cstr() );
		memcpy ( sCur, dFrom[i].cstr(), iLen );

		sCur[iLen] = MAGIC_SYNONYM_WHITESPACE;
		sCur += iLen+1;
	}
	*sCur++ = '\0';
	assert ( sCur-tSyn.m_sFrom.cstr()==iFromLen );

	// track synonym-only codepoints in map-from
	for ( ;; )
	{
		int iCode = IsUtf8() ? sphUTF8Decode(sFrom) : *sFrom++;
		if ( !iCode )
			break;
		if ( iCode>0 && !sphIsSpace(iCode) && !m_tLC.ToLower(iCode) )
			tHash.Add ( 1, iCode );
	}

	return true;
}


bool CSphTokenizerBase::LoadSynonyms ( const char * sFilename, const CSphEmbeddedFiles * pFiles, CSphString & sError )
{
	assert ( m_eMode!=SPH_CLONE_QUERY_LIGHTWEIGHT );
	m_dSynonyms.Reset ();

	CSphSynonymHash hSynonymOnly;

	if ( pFiles )
	{
		m_tSynFileInfo = pFiles->m_tSynonymFile;
		ARRAY_FOREACH ( i, pFiles->m_dSynonyms )
		{
			if ( !LoadSynonym ( (char*)pFiles->m_dSynonyms[i].cstr(), pFiles->m_tSynonymFile.m_sFilename.cstr(), i, hSynonymOnly, sError ) )
				sphWarning ( "%s", sError.cstr() );
		}
	} else
	{
		if ( !sFilename || !*sFilename )
			return true;

		GetFileStats ( sFilename, m_tSynFileInfo, NULL );

		CSphAutoreader tReader;
		if ( !tReader.Open ( sFilename, sError ) )
			return false;

		char sBuffer[1024];
		int iLine = 0;
		while ( tReader.GetLine ( sBuffer, sizeof(sBuffer) )>=0 )
		{
			iLine++;
			if ( !LoadSynonym ( sBuffer, sFilename, iLine, hSynonymOnly, sError ) )
				sphWarning ( "%s", sError.cstr() );
		}

		// sort the list
		m_dSynonyms.Sort ();
	}

	// build simple lookup table
	m_dSynStart.Resize ( 256 );
	m_dSynEnd.Resize ( 256 );
	for ( int i=0; i<256; i++ )
	{
		m_dSynStart[i] = INT_MAX;
		m_dSynEnd[i] = -INT_MAX;
	}
	ARRAY_FOREACH ( i, m_dSynonyms )
	{
		int iCh = *(BYTE*)( m_dSynonyms[i].m_sFrom.cstr() );
		m_dSynStart[iCh] = Min ( m_dSynStart[iCh], i );
		m_dSynEnd[iCh] = Max ( m_dSynEnd[iCh], i );
	}

	// add synonym-only remaps
	CSphVector<CSphRemapRange> dRemaps;
	dRemaps.Reserve ( hSynonymOnly.GetLength() );

	hSynonymOnly.IterateStart ();
	while ( hSynonymOnly.IterateNext() )
	{
		CSphRemapRange & tRange = dRemaps.Add ();
		tRange.m_iStart = tRange.m_iEnd = tRange.m_iRemapStart = hSynonymOnly.IterateGetKey();
	}

	m_tLC.AddRemaps ( dRemaps, FLAG_CODEPOINT_SYNONYM );
	return true;
}


void CSphTokenizerBase::WriteSynonyms ( CSphWriter & tWriter )
{
	tWriter.PutDword ( m_dSynonyms.GetLength() );
	ARRAY_FOREACH ( i, m_dSynonyms )
	{
		CSphString sFrom, sLine;
		sFrom = m_dSynonyms[i].m_sFrom;
		char * pFrom = (char*)sFrom.cstr();
		while ( pFrom && *pFrom )
		{
			if ( *pFrom==MAGIC_SYNONYM_WHITESPACE )
				*pFrom = ' ';
			pFrom++;
		}

		sFrom.Trim();
		sLine.SetSprintf ( "%s => %s", sFrom.cstr(), m_dSynonyms[i].m_sTo.cstr() );
		tWriter.PutString ( sLine );
	}
}


void CSphTokenizerBase::CloneBase ( const CSphTokenizerBase * pFrom, ESphTokenizerClone eMode )
{
	m_eMode = eMode;
	m_dSynonyms = pFrom->m_dSynonyms;
	m_dSynStart = pFrom->m_dSynStart;
	m_dSynEnd = pFrom->m_dSynEnd;
	m_tSettings = pFrom->m_tSettings;
	m_bHasBlend = pFrom->m_bHasBlend;
	m_uBlendVariants = pFrom->m_uBlendVariants;
	m_bBlendSkipPure = pFrom->m_bBlendSkipPure;
	m_bShortTokenFilter = ( eMode!=SPH_CLONE_INDEX );

	switch ( eMode )
	{
		case SPH_CLONE_INDEX:
			m_tLC = pFrom->m_tLC;
			break;

		case SPH_CLONE_QUERY:
		{
			m_tLC = pFrom->m_tLC;

			CSphVector<CSphRemapRange> dRemaps;
			CSphRemapRange Range;
			Range.m_iStart = Range.m_iEnd = Range.m_iRemapStart = '\\';
			dRemaps.Add ( Range );
			m_tLC.AddRemaps ( dRemaps, FLAG_CODEPOINT_SPECIAL );

			m_uBlendVariants = BLEND_TRIM_NONE;
			break;
		}

		case SPH_CLONE_QUERY_LIGHTWEIGHT:
		{
			// FIXME? avoid double lightweight clones, too?
			assert ( pFrom->m_eMode!=SPH_CLONE_INDEX );
			assert ( pFrom->m_tLC.ToLower ( '\\' ) & FLAG_CODEPOINT_SPECIAL );

			// lightweight tokenizer clone
			// copy 3 KB of lowercaser chunk pointers, but do NOT copy the table data
			SafeDeleteArray ( m_tLC.m_pData );
			m_tLC.m_iChunks = 0;
			m_tLC.m_pData = NULL;
			for ( int i=0; i<CSphLowercaser::CHUNK_COUNT; i++ )
				m_tLC.m_pChunk[i] = pFrom->m_tLC.m_pChunk[i];
			break;
		}
	}
}

uint64_t CSphTokenizerBase::GetSettingsFNV () const
{
	uint64_t uHash = ISphTokenizer::GetSettingsFNV();

	DWORD uFlags = 0;
	if ( m_bHasBlend )
		uFlags |= 1<<0;
	uHash = sphFNV64 ( (const BYTE *)&uFlags, sizeof(uFlags), uHash );

	return uHash;
}


void CSphTokenizerBase::SetBufferPtr ( const char * sNewPtr )
{
	assert ( (BYTE*)sNewPtr>=m_pBuffer && (BYTE*)sNewPtr<=m_pBufferMax );
	m_pCur = Min ( m_pBufferMax, Max ( m_pBuffer, (const BYTE*)sNewPtr ) );
	m_iAccum = 0;
	m_pAccum = m_sAccum;
	m_pTokenStart = m_pTokenEnd = NULL;
	m_pBlendStart = m_pBlendEnd = NULL;
}


template < bool IS_UTF8 >
int CSphTokenizerBase2<IS_UTF8>::SkipBlended()
{
	if ( !m_pBlendEnd )
		return 0;

	const BYTE * pMax = m_pBufferMax;
	m_pBufferMax = m_pBlendEnd;

	// loop until the blended token end
	int iBlended = 0; // how many blended subtokens we have seen so far
	int iAccum = 0; // how many non-blended chars in a row we have seen so far
	while ( m_pCur < m_pBufferMax )
	{
		int iCode = GetCodepoint();
		if ( iCode=='\\' )
			iCode = GetCodepoint(); // no boundary check, GetCP does it
		iCode = m_tLC.ToLower ( iCode ); // no -1 check, ToLower does it
		if ( iCode<0 )
			iCode = 0;
		if ( iCode & FLAG_CODEPOINT_BLEND )
			iCode = 0;
		if ( iCode & MASK_CODEPOINT )
		{
			iAccum++;
			continue;
		}
		if ( iAccum>=m_tSettings.m_iMinWordLen )
			iBlended++;
		iAccum = 0;
	}
	if ( iAccum>=m_tSettings.m_iMinWordLen )
		iBlended++;

	m_pBufferMax = pMax;
	return iBlended;
}


/// adjusts blending magic when we're about to return a token (any token)
/// returns false if current token should be skipped, true otherwise
bool CSphTokenizerBase::BlendAdjust ( const BYTE * pCur )
{
	// check if all we got is a bunch of blended characters (pure-blended case)
	if ( m_bBlended && !m_bNonBlended )
	{
		// we either skip this token, or pretend it was normal
		// in both cases, clear the flag
		m_bBlended = false;

		// do we need to skip it?
		if ( m_bBlendSkipPure )
		{
			m_pBlendStart = NULL;
			return false;
		}
	}
	m_bNonBlended = false;

	// adjust buffer pointers
	if ( m_bBlended && m_pBlendStart )
	{
		// called once per blended token, on processing start
		// at this point, full blended token is in the accumulator
		// and we're about to return it
		m_pCur = m_pBlendStart;
		m_pBlendEnd = pCur;
		m_pBlendStart = NULL;
		m_bBlendedPart = true;
	} else if ( pCur>=m_pBlendEnd )
	{
		// tricky bit, as at this point, token we're about to return
		// can either be a blended subtoken, or the next one
		m_bBlendedPart = ( m_pTokenStart!=NULL ) && ( m_pTokenStart<m_pBlendEnd );
		m_pBlendEnd = NULL;
		m_pBlendStart = NULL;
	} else if ( !m_pBlendEnd )
	{
		// we aren't re-parsing blended; so clear the "blended subtoken" flag
		m_bBlendedPart = false;
	}
	return true;
}


static inline void CopySubstring ( BYTE * pDst, const BYTE * pSrc, int iLen )
{
	while ( iLen-->0 && *pSrc )
		*pDst++ = *pSrc++;
	*pDst++ = '\0';
}


template < bool IS_UTF8 >
BYTE * CSphTokenizerBase2<IS_UTF8>::GetBlendedVariant ()
{
	// we can get called on several occasions
	// case 1, a new blended token was just accumulated
	if ( m_bBlended && !m_bBlendAdd )
	{
		// fast path for the default case (trim_none)
		if ( m_uBlendVariants==BLEND_TRIM_NONE )
			return m_sAccum;

		// analyze the full token, find non-blended bounds
		m_iBlendNormalStart = -1;
		m_iBlendNormalEnd = -1;

		// OPTIMIZE? we can skip this based on non-blended flag from adjust
		const BYTE * p = m_sAccum;
		while ( *p )
		{
			int iLast = (int)( p-m_sAccum );
			int iCode = IS_UTF8
				? sphUTF8Decode ( p )
				: *p++;
			if (!( m_tLC.ToLower ( iCode ) & FLAG_CODEPOINT_BLEND ))
			{
				m_iBlendNormalEnd = (int)( p-m_sAccum );
				if ( m_iBlendNormalStart<0 )
					m_iBlendNormalStart = iLast;
			}
		}

		// build todo mask
		// check and revert a few degenerate cases
		m_uBlendVariantsPending = m_uBlendVariants;
		if ( m_uBlendVariantsPending & BLEND_TRIM_BOTH )
		{
			if ( m_iBlendNormalStart<0 )
			{
				// no heading blended; revert BOTH to TAIL
				m_uBlendVariantsPending &= ~BLEND_TRIM_BOTH;
				m_uBlendVariantsPending |= BLEND_TRIM_TAIL;
			} else if ( m_iBlendNormalEnd<0 )
			{
				// no trailing blended; revert BOTH to HEAD
				m_uBlendVariantsPending &= ~BLEND_TRIM_BOTH;
				m_uBlendVariantsPending |= BLEND_TRIM_HEAD;
			}
		}
		if ( m_uBlendVariantsPending & BLEND_TRIM_HEAD )
		{
			// either no heading blended, or pure blended; revert HEAD to NONE
			if ( m_iBlendNormalStart<=0 )
			{
				m_uBlendVariantsPending &= ~BLEND_TRIM_HEAD;
				m_uBlendVariantsPending |= BLEND_TRIM_NONE;
			}
		}
		if ( m_uBlendVariantsPending & BLEND_TRIM_TAIL )
		{
			// either no trailing blended, or pure blended; revert TAIL to NONE
			if ( m_iBlendNormalEnd<=0 || m_sAccum[m_iBlendNormalEnd]==0 )
			{
				m_uBlendVariantsPending &= ~BLEND_TRIM_TAIL;
				m_uBlendVariantsPending |= BLEND_TRIM_NONE;
			}
		}

		// ok, we are going to return a few variants after all, flag that
		// OPTIMIZE? add fast path for "single" variants?
		m_bBlendAdd = true;
		assert ( m_uBlendVariantsPending );

		// we also have to stash the original blended token
		// because accumulator contents may get trashed by caller (say, when stemming)
		strncpy ( (char*)m_sAccumBlend, (char*)m_sAccum, sizeof(m_sAccumBlend) );
	}

	// case 2, caller is checking for pending variants, have we even got any?
	if ( !m_bBlendAdd )
		return NULL;

	// handle trim_none
	// this MUST be the first handler, so that we could avoid copying below, and just return the original accumulator
	if ( m_uBlendVariantsPending & BLEND_TRIM_NONE )
	{
		m_uBlendVariantsPending &= ~BLEND_TRIM_NONE;
		m_bBlended = true;
		return m_sAccum;
	}

	// handle trim_both
	if ( m_uBlendVariantsPending & BLEND_TRIM_BOTH )
	{
		m_uBlendVariantsPending &= ~BLEND_TRIM_BOTH;
		if ( m_iBlendNormalStart<0 )
			m_uBlendVariantsPending |= BLEND_TRIM_TAIL; // no heading blended; revert BOTH to TAIL
		else if ( m_iBlendNormalEnd<0 )
			m_uBlendVariantsPending |= BLEND_TRIM_HEAD; // no trailing blended; revert BOTH to HEAD
		else
		{
			assert ( m_iBlendNormalStart<m_iBlendNormalEnd );
			CopySubstring ( m_sAccum, m_sAccumBlend+m_iBlendNormalStart, m_iBlendNormalEnd-m_iBlendNormalStart );
			m_bBlended = true;
			return m_sAccum;
		}
	}

	// handle TRIM_HEAD
	if ( m_uBlendVariantsPending & BLEND_TRIM_HEAD )
	{
		m_uBlendVariantsPending &= ~BLEND_TRIM_HEAD;
		if ( m_iBlendNormalStart>=0 )
		{
			// FIXME! need we check for overshorts?
			CopySubstring ( m_sAccum, m_sAccumBlend+m_iBlendNormalStart, sizeof(m_sAccum) );
			m_bBlended = true;
			return m_sAccum;
		}
	}

	// handle TRIM_TAIL
	if ( m_uBlendVariantsPending & BLEND_TRIM_TAIL )
	{
		m_uBlendVariantsPending &= ~BLEND_TRIM_TAIL;
		if ( m_iBlendNormalEnd>0 )
		{
			// FIXME! need we check for overshorts?
			CopySubstring ( m_sAccum, m_sAccumBlend, m_iBlendNormalEnd );
			m_bBlended = true;
			return m_sAccum;
		}
	}

	// all clear, no more variants to go
	m_bBlendAdd = false;
	return NULL;
}


static inline bool IsCapital ( int iCh )
{
	return iCh>='A' && iCh<='Z';
}


static inline bool IsWhitespace ( BYTE c )
{
	return ( c=='\0' || c==' ' || c=='\t' || c=='\r' || c=='\n' );
}


static inline bool IsWhitespace ( int c )
{
	return ( c=='\0' || c==' ' || c=='\t' || c=='\r' || c=='\n' );
}


static inline bool IsBoundary ( BYTE c, bool bPhrase )
{
	// FIXME? sorta intersects with specials
	// then again, a shortened-down list (more strict syntax) is reasonble here too
	return IsWhitespace(c) || c=='"' || ( !bPhrase && ( c=='(' || c==')' || c=='|' ) );
}


int CSphTokenizerBase::CodepointArbitrationI ( int iCode )
{
	if ( !m_bDetectSentences )
		return iCode;

	// detect sentence boundaries
	// FIXME! should use charset_table (or add a new directive) and support languages other than English
	int iSymbol = iCode & MASK_CODEPOINT;
	if ( iSymbol=='?' || iSymbol=='!' )
	{
		// definitely a sentence boundary
		return MAGIC_CODE_SENTENCE | FLAG_CODEPOINT_SPECIAL;
	}

	if ( iSymbol=='.' )
	{
		// inline dot ("in the U.K and"), not a boundary
		bool bInwordDot = ( sphIsAlpha ( m_pCur[0] ) || m_pCur[0]==',' );

		// followed by a small letter or an opening paren, not a boundary
		// FIXME? might want to scan for more than one space
		// Yoyodine Inc. exists ...
		// Yoyodine Inc. (the company) ..
		bool bInphraseDot = ( sphIsSpace ( m_pCur[0] )
			&& ( ( 'a'<=m_pCur[1] && m_pCur[1]<='z' )
				|| ( m_pCur[1]=='(' && 'a'<=m_pCur[2] && m_pCur[2]<='z' ) ) );

		// preceded by something that looks like a middle name, opening first name, salutation
		bool bMiddleName = false;
		switch ( m_iAccum )
		{
			case 1:
				// 1-char capital letter
				// example: J. R. R. Tolkien, who wrote Hobbit ...
				// example: John D. Doe ...
				bMiddleName = IsCapital ( m_pCur[-2] );
				break;
			case 2:
				// 2-char token starting with a capital
				if ( IsCapital ( m_pCur[-3] ) )
				{
					// capital+small
					// example: Known as Mr. Doe ...
					if ( !IsCapital ( m_pCur[-2] ) )
						bMiddleName = true;

					// known capital+capital (MR, DR, MS)
					if (
						( m_pCur[-3]=='M' && m_pCur[-2]=='R' ) ||
						( m_pCur[-3]=='M' && m_pCur[-2]=='S' ) ||
						( m_pCur[-3]=='D' && m_pCur[-2]=='R' ) )
							bMiddleName = true;
				}
				break;
			case 3:
				// preceded by a known 3-byte token (MRS, DRS)
				// example: Survived by Mrs. Doe ...
				if ( ( m_sAccum[0]=='m' || m_sAccum[0]=='d' ) && m_sAccum[1]=='r' && m_sAccum[2]=='s' )
					bMiddleName = true;
				break;
		}

		if ( !bInwordDot && !bInphraseDot && !bMiddleName )
		{
			// sentence boundary
			return MAGIC_CODE_SENTENCE | FLAG_CODEPOINT_SPECIAL;
		} else
		{
			// just a character
			if ( ( iCode & MASK_FLAGS )==FLAG_CODEPOINT_SPECIAL )
				return 0; // special only, not dual? then in this context, it is a separator
			else
				return iCode & ~( FLAG_CODEPOINT_SPECIAL | FLAG_CODEPOINT_DUAL ); // perhaps it was blended, so return the original code
		}
	}

	// pass-through
	return iCode;
}


int CSphTokenizerBase::CodepointArbitrationQ ( int iCode, bool bWasEscaped, BYTE uNextByte )
{
	if ( iCode & FLAG_CODEPOINT_NGRAM )
		return iCode; // ngrams are handled elsewhere

	int iSymbol = iCode & MASK_CODEPOINT;

	// codepoints can't be blended and special at the same time
	if ( ( iCode & FLAG_CODEPOINT_BLEND ) && ( iCode & FLAG_CODEPOINT_SPECIAL ) )
	{
		bool bBlend =
			bWasEscaped || // escaped characters should always act as blended
			( m_bPhrase && !sphIsModifier ( iSymbol ) && iSymbol!='"' ) || // non-modifier special inside phrase
			( m_iAccum && ( iSymbol=='@' || iSymbol=='/' || iSymbol=='-' ) ); // some specials in the middle of a token

		// clear special or blend flags
		iCode &= bBlend
			? ~( FLAG_CODEPOINT_DUAL | FLAG_CODEPOINT_SPECIAL )
			: ~( FLAG_CODEPOINT_DUAL | FLAG_CODEPOINT_BLEND );
	}

	// escaped specials are not special
	// dash and dollar inside the word are not special (however, single opening modifier is not a word!)
	// non-modifier specials within phrase are not special
	bool bDashInside = ( m_iAccum && iSymbol=='-' && !( m_iAccum==1 && sphIsModifier ( m_sAccum[0] ) ));
	if ( iCode & FLAG_CODEPOINT_SPECIAL )
		if ( bWasEscaped
			|| bDashInside
			|| ( m_iAccum && iSymbol=='$' && !IsBoundary ( uNextByte, m_bPhrase ) )
			|| ( m_bPhrase && iSymbol!='"' && !sphIsModifier ( iSymbol ) ) )
	{
		if ( iCode & FLAG_CODEPOINT_DUAL )
			iCode &= ~( FLAG_CODEPOINT_SPECIAL | FLAG_CODEPOINT_DUAL );
		else if ( bDashInside && ( iCode & FLAG_CODEPOINT_SYNONYM ) )
			// if we return zero here, we will break the tokens like 'Ms-Dos'
			iCode &= ~( FLAG_CODEPOINT_SPECIAL );
		else
			iCode = 0;
	}

	// if we didn't remove special by now, it must win
	if ( iCode & FLAG_CODEPOINT_DUAL )
	{
		assert ( iCode & FLAG_CODEPOINT_SPECIAL );
		iCode = iSymbol | FLAG_CODEPOINT_SPECIAL;
	}

	// ideally, all conflicts must be resolved here
	// well, at least most
	assert ( sphBitCount ( iCode & MASK_FLAGS )<=1
		|| ( iCode & FLAG_CODEPOINT_SYNONYM ) );
	return iCode;
}


enum SynCheck_e
{
	SYNCHECK_LESS,
	SYNCHECK_PARTIAL,
	SYNCHECK_EXACT,
	SYNCHECK_GREATER
};


static inline SynCheck_e SynCheckPrefix ( const CSphSynonym & tCandidate, int iOff, const BYTE * sCur, int iBytes, bool bMaybeSeparator )
{
	const BYTE * sCand = ( (const BYTE*)tCandidate.m_sFrom.cstr() ) + iOff;

	while ( iBytes-->0 )
	{
		if ( *sCand!=*sCur )
		{
			// incoming synonym-only char vs. ending sequence (eg. 2nd slash in "OS/2/3"); we actually have a match
			if ( bMaybeSeparator && sCand[0]==MAGIC_SYNONYM_WHITESPACE && sCand[1]=='\0' )
				return SYNCHECK_EXACT;

			// otherwise, it is a mismatch
			return ( *sCand<*sCur ) ? SYNCHECK_LESS : SYNCHECK_GREATER;
		}
		sCand++;
		sCur++;
	}

	// full match after a full separator
	if ( sCand[0]=='\0' )
		return SYNCHECK_EXACT;

	// full match after my last synonym-only char
	if ( bMaybeSeparator && sCand[0]==MAGIC_SYNONYM_WHITESPACE && sCand[1]=='\0' )
		return SYNCHECK_EXACT;

	// otherwise, partial match so far
	return SYNCHECK_PARTIAL;
}


#if !USE_WINDOWS
#define __forceinline inline
#endif

static __forceinline bool IsSeparator ( int iFolded, bool bFirst )
{
	// eternal separator
	if ( iFolded<0 || ( iFolded & MASK_CODEPOINT )==0 )
		return true;

	// just a codepoint
	if (!( iFolded & MASK_FLAGS ))
		return false;

	// any magic flag, besides dual
	if (!( iFolded & FLAG_CODEPOINT_DUAL ))
		return true;

	// FIXME? n-grams currently also set dual
	if ( iFolded & FLAG_CODEPOINT_NGRAM )
		return true;

	// dual depends on position
	return bFirst;
}

// handles escaped specials that are not in the character set
// returns true if the codepoint should be processed as a simple codepoint,
// returns false if it should be processed as a whitespace
// for example: aaa\!bbb => aaa bbb
static inline bool Special2Simple ( int & iCodepoint )
{
	if ( ( iCodepoint & FLAG_CODEPOINT_DUAL ) || !( iCodepoint & FLAG_CODEPOINT_SPECIAL ) )
	{
		iCodepoint &= ~( FLAG_CODEPOINT_SPECIAL | FLAG_CODEPOINT_DUAL );
		return true;
	}

	return false;
}


template < bool IS_UTF8 >
BYTE * CSphTokenizerBase2<IS_UTF8>::GetTokenSyn ( bool bQueryMode )
{
	assert ( m_dSynonyms.GetLength() );

	m_bWasSynonym = false;
	const BYTE * pCur;

	m_bTokenBoundary = false;
	for ( ;; )
	{
		// initialize accumulators and range
		const BYTE * pFirstSeparator = NULL;

		m_iAccum = 0;
		m_pAccum = m_sAccum;

		int iSynStart = 0;
		int iSynEnd = m_dSynonyms.GetLength()-1;
		int iSynOff = 0;

		int iLastCodepoint = 0;
		int iLastFolded = 0;
		const BYTE * pRescan = NULL;

		int iExact = -1;
		const BYTE * pExact = NULL;

		// main refinement loop
		for ( ;; )
		{
			// store current position (to be able to restart from it on folded boundary)
			pCur = m_pCur;

			// get next codepoint, fold it, lookup flags
			int iCode;
			int iFolded;
			if ( pCur<m_pBufferMax && *pCur<128 )
			{
				// fastpath, ascii7 is identical in both SBCS and UTF8 encodings
				iCode = *m_pCur++;
				iFolded = m_tLC.m_pChunk[0][iCode];
			} else
			{
				iCode = GetCodepoint(); // advances m_pCur
				iFolded = m_tLC.ToLower ( iCode );
			}

			// handle early-out
			if ( iCode<0 )
			{
				// eof at token start? we're done
				if ( iSynOff==0 )
					return NULL;

				// eof after whitespace? we already checked the candidate last time, so break
				if ( iLastFolded==0 )
					break;
			}

			// handle boundaries
			if ( m_bBoundary && ( iFolded==0 ) ) m_bTokenBoundary = true;
			m_bBoundary = ( iFolded & FLAG_CODEPOINT_BOUNDARY )!=0;

			// handle escapes
			if ( bQueryMode )
			{
				if ( iCode=='\\' && iLastCodepoint!='\\' )
				{
					iLastCodepoint = iCode;
					continue;
				} else if ( iLastCodepoint=='\\' && ( iFolded & FLAG_CODEPOINT_SYNONYM ) && ( iFolded & FLAG_CODEPOINT_SPECIAL ) )
				{
					iFolded &= ~FLAG_CODEPOINT_SPECIAL;

				} else if ( iLastCodepoint=='\\' && !Special2Simple ( iFolded ) )
				{
					iLastCodepoint = 0;
					continue;
				}

				iLastCodepoint = iCode;
			}

			// skip continuous whitespace
			// (must be here, because boundaries and escapes might fold to whitespace)
			if ( iLastFolded==0 && iFolded==0 )
				continue;

			if ( bQueryMode )
				iFolded = CodepointArbitrationQ ( iFolded, false, *m_pCur );
			else if ( m_bDetectSentences )
				iFolded = CodepointArbitrationI ( iFolded );

			iLastFolded = iFolded;

			if ( m_iAccum==0 )
				m_pTokenStart = pCur;

			// handle specials at the very word start
			if ( ( iFolded & FLAG_CODEPOINT_SPECIAL ) && m_iAccum==0 )
			{
				m_bWasSpecial = !( iFolded & FLAG_CODEPOINT_NGRAM );

				AccumCodepoint ( iFolded & MASK_CODEPOINT );
				*m_pAccum = '\0';

				m_iLastTokenLen = 1;
				m_pTokenStart = pCur;
				m_pTokenEnd = m_pCur;
				return m_sAccum;
			}

			// handle specials
			bool bJustSpecial = ( iFolded & FLAG_CODEPOINT_SPECIAL )
				&& !( iFolded & FLAG_CODEPOINT_DUAL ) // OPTIMIZE?
				&& !( iFolded & FLAG_CODEPOINT_SYNONYM ); // OPTIMIZE?

			// if candidate starts with something special, and turns out to be not a synonym,
			// we will need to rescan from current position later
			if ( iSynOff==0 )
				pRescan = IsSeparator ( iFolded, true ) ? m_pCur : NULL;

			// accumulate folded token
			if ( !pFirstSeparator )
			{
				if ( IsSeparator ( iFolded, m_iAccum==0 ) )
				{
					if ( m_iAccum )
						pFirstSeparator = pCur;
				} else
				{
					if ( m_iAccum==0 )
						m_pTokenStart = pCur;

					AccumCodepoint ( iFolded & MASK_CODEPOINT );
				}
			}

			// accumulate next raw synonym symbol to refine
			// note that we need a special check for whitespace here, to avoid "MS*DOS" being treated as "MS DOS" synonym
			BYTE sTest[4];
			int iTest;

			int iMasked = ( iCode & MASK_CODEPOINT );
			if ( iFolded<=0 || bJustSpecial )
			{
				sTest[0] = MAGIC_SYNONYM_WHITESPACE;
				iTest = 1;

				if (!( iMasked==' ' || iMasked=='\t' ))
				{
					sTest[1] = '\0';
					iTest = 2;
				}
			} else
			{
				if ( IsUtf8() )
				{
					iTest = sphUTF8Encode ( sTest, iMasked );
				} else
				{
					iTest = 1;
					sTest[0] = BYTE(iMasked);
				}
			}

			// refine synonyms range
			#define LOC_RETURN_SYNONYM(_idx) \
			{ \
				m_pTokenEnd = m_iAccum ? pCur : m_pCur; \
				if ( bJustSpecial || ( iFolded & FLAG_CODEPOINT_SPECIAL )!=0 ) m_pCur = pCur; \
				strncpy ( (char*)m_sAccum, m_dSynonyms[_idx].m_sTo.cstr(), sizeof(m_sAccum) ); \
				m_iLastTokenLen = m_dSynonyms[_idx].m_iToLen; \
				m_bWasSynonym = true; \
				return m_sAccum; \
			}

			#define LOC_REFINE_BREAK() \
			{ \
				if ( iExact>=0 ) { m_pCur = pCur = pExact; LOC_RETURN_SYNONYM ( iExact ); } \
				break; \
			}

			// if this is the first symbol, use prebuilt lookup table to speedup initial range search
			if ( iSynOff==0 )
			{
				iSynStart = m_dSynStart[sTest[0]];
				iSynEnd = m_dSynEnd[sTest[0]];
				if ( iSynStart>iSynEnd )
					break;
			}

			// this is to catch intermediate separators (eg. "OS/2/3")
			bool bMaybeSeparator = ( iFolded & FLAG_CODEPOINT_SYNONYM )!=0 || ( iFolded<0 );

			SynCheck_e eStart = SynCheckPrefix ( m_dSynonyms[iSynStart], iSynOff, sTest, iTest, bMaybeSeparator );
			if ( eStart==SYNCHECK_EXACT )
			{
				if ( iSynStart==iSynEnd ) LOC_RETURN_SYNONYM ( iSynStart );
				iExact = iSynStart;
				pExact = pCur;
			}
			if ( eStart==SYNCHECK_GREATER || ( iSynStart==iSynEnd && eStart!=SYNCHECK_PARTIAL ) )
				LOC_REFINE_BREAK();

			SynCheck_e eEnd = SynCheckPrefix ( m_dSynonyms[iSynEnd], iSynOff, sTest, iTest, bMaybeSeparator );
			if ( eEnd==SYNCHECK_LESS )
				LOC_REFINE_BREAK();
			if ( eEnd==SYNCHECK_EXACT )
			{
				iExact = iSynEnd;
				pExact = pCur;
			}


			// refine left boundary
			if ( eStart!=SYNCHECK_PARTIAL && eStart!=SYNCHECK_EXACT )
			{
				assert ( eStart==SYNCHECK_LESS );

				int iL = iSynStart;
				int iR = iSynEnd;
				SynCheck_e eL = eStart;
				SynCheck_e eR = eEnd;

				while ( iR-iL>1 )
				{
					int iM = iL + (iR-iL)/2;
					SynCheck_e eMid = SynCheckPrefix ( m_dSynonyms[iM], iSynOff, sTest, iTest, bMaybeSeparator );

					if ( eMid==SYNCHECK_LESS )
					{
						iL = iM;
						eL = eMid;
					} else
					{
						iR = iM;
						eR = eMid;
					}
				}

				assert ( eL==SYNCHECK_LESS );
				assert ( eR!=SYNCHECK_LESS );
				assert ( iR-iL==1 );

				if ( eR==SYNCHECK_GREATER )					LOC_REFINE_BREAK();
				if ( eR==SYNCHECK_EXACT && iR==iSynEnd )	LOC_RETURN_SYNONYM ( iR );

				assert ( eR==SYNCHECK_PARTIAL || eR==SYNCHECK_EXACT );
				iSynStart = iR;
				eStart = eR;
			}

			// refine right boundary
			if ( eEnd!=SYNCHECK_PARTIAL && eEnd!=SYNCHECK_EXACT )
			{
				assert ( eEnd==SYNCHECK_GREATER );

				int iL = iSynStart;
				int iR = iSynEnd;
				SynCheck_e eL = eStart;
				SynCheck_e eR = eEnd;

				while ( iR-iL>1 )
				{
					int iM = iL + (iR-iL)/2;
					SynCheck_e eMid = SynCheckPrefix ( m_dSynonyms[iM], iSynOff, sTest, iTest, bMaybeSeparator );

					if ( eMid==SYNCHECK_GREATER )
					{
						iR = iM;
						eR = eMid;
					} else
					{
						iL = iM;
						eL = eMid;
					}
				}

				assert ( eR==SYNCHECK_GREATER );
				assert ( eL!=SYNCHECK_GREATER );
				assert ( iR-iL==1 );

				if ( eL==SYNCHECK_LESS )					LOC_REFINE_BREAK();
				if ( eL==SYNCHECK_EXACT && iL==iSynStart )	LOC_RETURN_SYNONYM ( iL );

				assert ( eL==SYNCHECK_PARTIAL || eL==SYNCHECK_EXACT );
				iSynEnd = iL;
				eEnd = eL;
			}

			// handle eof
			if ( iCode<0 )
				break;

			// we still have a partial synonym match, continue;
			iSynOff += iTest;
		}

		// at this point, that was not a synonym
		if ( pRescan )
		{
			m_pCur = pRescan;
			continue;
		}

		// at this point, it also started with a valid char
		assert ( m_iAccum>0 );

		// find the proper separator
		if ( !pFirstSeparator )
		{
			// if there was none, scan until found
			for ( ;; )
			{
				pCur = m_pCur;

				int iCode;
				int iFolded;
				if ( pCur<m_pBufferMax && *pCur<128 )
				{
					// fastpath, ascii7 is identical in both SBCS and UTF8 encodings
					iCode = *m_pCur++;
					iFolded = m_tLC.m_pChunk[0][iCode];
				} else
				{
					iCode = GetCodepoint(); // advances m_pCur
					iFolded = m_tLC.ToLower ( iCode );
				}

				if ( iFolded<0 )
					break; // eof

				if ( bQueryMode && iCode=='\\' )
				{
					iCode = GetCodepoint(); // advances m_pCur
					iFolded = m_tLC.ToLower ( iCode );
					if ( iFolded<0 )
						break;
					if ( !Special2Simple ( iFolded ) )
						break;
				}

				if ( bQueryMode )
					iFolded = CodepointArbitrationQ ( iFolded, false, *m_pCur );
				else if ( m_bDetectSentences )
					iFolded = CodepointArbitrationI ( iFolded );

				if ( IsSeparator ( iFolded, false ) )
				{
					if ( iFolded!=0 )
						m_pCur = pCur; // force rescan
					break;
				}

				// the hottest accumulation point
				// so do this manually, no function calls, that is quickest
				bool bFit = ( m_iAccum<SPH_MAX_WORD_LEN );
				if_const ( IS_UTF8 )
					bFit &= ( m_pAccum-m_sAccum+SPH_MAX_UTF8_BYTES<=(int)sizeof(m_sAccum) );

				if ( bFit )
				{
					m_iAccum++;
					if_const ( IS_UTF8 )
					{
						iFolded &= MASK_CODEPOINT;
						SPH_UTF8_ENCODE ( m_pAccum, iFolded );
					} else
						*m_pAccum++ = BYTE(iFolded);
				}
			}
		} else
		{
			// if there was, token is ready but we should restart from that separator
			m_pCur = pFirstSeparator;
			pCur = m_pCur;
		}

		// return accumulated token
		if ( m_iAccum<m_tSettings.m_iMinWordLen )
		{
			if ( m_bShortTokenFilter )
			{
				*m_pAccum = '\0';

				if ( ShortTokenFilter ( m_sAccum, m_iAccum ) )
				{
					m_iLastTokenLen = m_iAccum;
					m_pTokenEnd = pCur;
					m_iAccum = 0;
					return m_sAccum;
				}
			}

			if ( m_iAccum )
				m_iOvershortCount++;

			m_iAccum = 0;
			continue;
		}

		*m_pAccum = '\0';
		m_iLastTokenLen = m_iAccum;
		m_pTokenEnd = pCur;
		return m_sAccum;
	}
}


bool ISphTokenizer::RemapCharacters ( const char * sConfig, DWORD uFlags, const char * sSource, bool bCanRemap, CSphString & sError )
{
	// parse
	CSphVector<CSphRemapRange> dRemaps;
	CSphCharsetDefinitionParser tParser;
	if ( !tParser.Parse ( sConfig, dRemaps ) )
	{
		sError = tParser.GetLastError();
		return false;
	}

	// check
	ARRAY_FOREACH ( i, dRemaps )
	{
		const CSphRemapRange & r = dRemaps[i];

		if ( !bCanRemap && r.m_iStart!=r.m_iRemapStart )
		{
			sError.SetSprintf ( "%s characters must not be remapped (map-from=U+%x, map-to=U+%x)",
				sSource, r.m_iStart, r.m_iRemapStart );
			return false;
		}

		for ( int j=r.m_iStart; j<=r.m_iEnd; j++ )
			if ( m_tLC.ToLower(j) )
		{
			sError.SetSprintf ( "%s characters must not be referenced anywhere else (code=U+%x)", sSource, j );
			return false;
		}

		if ( bCanRemap )
			for ( int j=r.m_iRemapStart; j<=r.m_iRemapStart + r.m_iEnd - r.m_iStart; j++ )
				if ( m_tLC.ToLower(j) )
		{
			sError.SetSprintf ( "%s characters must not be referenced anywhere else (code=U+%x)", sSource, j );
			return false;
		}
	}

	// add mapping
	m_tLC.AddRemaps ( dRemaps, uFlags );
	return true;
}

bool ISphTokenizer::SetBoundary ( const char * sConfig, CSphString & sError )
{
	return RemapCharacters ( sConfig, FLAG_CODEPOINT_BOUNDARY, "phrase boundary", false, sError );
}

bool ISphTokenizer::SetIgnoreChars ( const char * sConfig, CSphString & sError )
{
	return RemapCharacters ( sConfig, FLAG_CODEPOINT_IGNORE, "ignored", false, sError );
}

bool ISphTokenizer::SetBlendChars ( const char * sConfig, CSphString & sError )
{
	return sConfig ? RemapCharacters ( sConfig, FLAG_CODEPOINT_BLEND, "blend", true, sError ) : false;
}


static bool sphStrncmp ( const char * sCheck, int iCheck, const char * sRef )
{
	return ( iCheck==(int)strlen(sRef) && memcmp ( sCheck, sRef, iCheck )==0 );
}


bool ISphTokenizer::SetBlendMode ( const char * sMode, CSphString & sError )
{
	if ( !sMode || !*sMode )
	{
		m_uBlendVariants = BLEND_TRIM_NONE;
		m_bBlendSkipPure = false;
		return true;
	}

	m_uBlendVariants = 0;
	const char * p = sMode;
	while ( *p )
	{
		while ( !sphIsAlpha(*p) )
			p++;
		if ( !*p )
			break;

		const char * sTok = p;
		while ( sphIsAlpha(*p) )
			p++;
		if ( sphStrncmp ( sTok, p-sTok, "trim_none" ) )
			m_uBlendVariants |= BLEND_TRIM_NONE;
		else if ( sphStrncmp ( sTok, p-sTok, "trim_head" ) )
			m_uBlendVariants |= BLEND_TRIM_HEAD;
		else if ( sphStrncmp ( sTok, p-sTok, "trim_tail" ) )
			m_uBlendVariants |= BLEND_TRIM_TAIL;
		else if ( sphStrncmp ( sTok, p-sTok, "trim_both" ) )
			m_uBlendVariants |= BLEND_TRIM_BOTH;
		else if ( sphStrncmp ( sTok, p-sTok, "skip_pure" ) )
			m_bBlendSkipPure = true;
		else
		{
			sError.SetSprintf ( "unknown blend_mode option near '%s'", sTok );
			return false;
		}
	}

	if ( !m_uBlendVariants )
	{
		sError.SetSprintf ( "blend_mode must define at least one variant to index" );
		m_uBlendVariants = BLEND_TRIM_NONE;
		m_bBlendSkipPure = false;
		return false;
	}
	return true;
}

/////////////////////////////////////////////////////////////////////////////

template < bool IS_QUERY >
CSphTokenizer_SBCS<IS_QUERY>::CSphTokenizer_SBCS ()
{
	CSphString sTmp;
	SetCaseFolding ( SPHINX_DEFAULT_SBCS_TABLE, sTmp );
}


template < bool IS_QUERY >
void CSphTokenizer_SBCS<IS_QUERY>::SetBuffer ( const BYTE * sBuffer, int iLength )
{
	// check that old one is over and that new length is sane
	assert ( iLength>=0 );

	// set buffer
	m_pBuffer = sBuffer;
	m_pBufferMax = sBuffer + iLength;
	m_pCur = sBuffer;
	m_pTokenStart = m_pTokenEnd = NULL;
	m_pBlendStart = m_pBlendEnd = NULL;

	m_iOvershortCount = 0;
	m_bBoundary = m_bTokenBoundary = false;
}


template < bool IS_QUERY >
BYTE * CSphTokenizer_SBCS<IS_QUERY>::GetToken ()
{
	m_bWasSpecial = false;
	m_bBlended = false;
	m_iOvershortCount = 0;
	m_bTokenBoundary = false;

	if ( m_dSynonyms.GetLength() )
		return GetTokenSyn ( IS_QUERY );

	// return pending blending variants
	BYTE * pVar = GetBlendedVariant ();
	if ( pVar )
		return pVar;
	m_bBlendedPart = ( m_pBlendEnd!=NULL );

	for ( ;; )
	{
		// memorize buffer start
		const BYTE * pCur = m_pCur;

		// get next codepoint, real or virtual
		int iCodepoint = 0;
		int iCode = 0;

		bool bWasEscaped = false; // whether current char was escaped
		if ( m_pCur<m_pBufferMax )
		{
			// get next codepoint
			iCodepoint = *m_pCur++;
			iCode = m_tLC.ToLower ( iCodepoint );

			// handle escaping
			if_const ( IS_QUERY && iCodepoint=='\\' )
			{
				if ( m_pCur<m_pBufferMax )
				{
					// fetch, fold, and then forcibly demote special
					iCodepoint = *m_pCur++;
					iCode = m_tLC.ToLower ( iCodepoint );
					if ( !Special2Simple ( iCode ) )
						iCode = 0;
					bWasEscaped = true;

				} else
				{
					// stray slash on a buffer end
					// handle it as a separator
					iCode = 0;
				}
			}

		} else
		{
			// out of buffer
			// but still need to handle short tokens
			if ( m_iAccum<m_tSettings.m_iMinWordLen )
			{
				bool bShortToken = false;
				if ( m_bShortTokenFilter )
				{
					m_sAccum[m_iAccum] = '\0';
					if ( ShortTokenFilter ( m_sAccum, m_iAccum ) )
						bShortToken = true;
				}

				if ( !bShortToken )
				{
					if ( m_iAccum )
						m_iOvershortCount++;
					m_iAccum = 0;
					m_iLastTokenLen = 0;
					BlendAdjust ( pCur );
					return NULL;
				}
			}
		}

		// handle all the flags..
		if_const ( IS_QUERY )
			iCode = CodepointArbitrationQ ( iCode, bWasEscaped, *m_pCur );
		else if ( m_bDetectSentences )
			iCode = CodepointArbitrationI ( iCode );

		// handle ignored chars
		if ( iCode & FLAG_CODEPOINT_IGNORE )
			continue;

		// handle blended characters
		if ( iCode & FLAG_CODEPOINT_BLEND )
		{
			if ( m_pBlendEnd )
				iCode = 0;
			else
			{
				m_bBlended = true;
				m_pBlendStart = m_iAccum ? m_pTokenStart : pCur;
			}
		}

		// handle whitespace and boundary
		if ( m_bBoundary && ( iCode==0 ) )
		{
			m_bTokenBoundary = true;
			m_iBoundaryOffset = pCur - m_pBuffer - 1;
		}
		m_bBoundary = ( iCode & FLAG_CODEPOINT_BOUNDARY )!=0;
		if ( iCode==0 || m_bBoundary )
		{
			if ( m_iAccum<m_tSettings.m_iMinWordLen )
			{
				bool bShortToken = false;
				if ( m_bShortTokenFilter )
				{
					m_sAccum[m_iAccum] = '\0';
					if ( ShortTokenFilter ( m_sAccum, m_iAccum ) )
						bShortToken = true;
				}

				if ( !bShortToken )
				{
					if ( m_iAccum )
						m_iOvershortCount++;

					m_iAccum = 0;
					BlendAdjust ( pCur );
					continue;
				}
			}

			m_iLastTokenLen = m_iAccum;
			m_sAccum[m_iAccum] = '\0';
			m_iAccum = 0;
			m_pTokenEnd = pCur>=m_pBufferMax ? m_pCur : pCur;
			if ( !BlendAdjust ( pCur ) )
				continue;
			if ( m_bBlended )
				return GetBlendedVariant();
			return m_sAccum;
		}

		// handle specials
		bool bSpecial = ( iCode & FLAG_CODEPOINT_SPECIAL )!=0;
		bool bNoBlend = !( iCode & FLAG_CODEPOINT_BLEND );
		iCode &= MASK_CODEPOINT;
		if ( bSpecial )
		{
			// skip short words
			if ( m_iAccum<m_tSettings.m_iMinWordLen )
			{
				if ( m_iAccum )
					m_iOvershortCount++;

				bool bShortToken = false;
				if ( m_bShortTokenFilter )
				{
					m_sAccum[m_iAccum] = '\0';
					if ( ShortTokenFilter ( m_sAccum, m_iAccum ) )
						bShortToken = true;
				}

				if ( !bShortToken )
				{
					if ( m_iAccum )
						m_iOvershortCount++;

					m_iAccum = 0;
				}
			}

			m_pTokenEnd = m_pCur;

			if ( m_iAccum==0 )
			{
				// nice standalone special
				m_iLastTokenLen = 1;
				m_sAccum[0] = (BYTE)iCode;
				m_sAccum[1] = '\0';
				m_pTokenStart = pCur;
				m_bWasSpecial = true;

			} else
			{
				// flush prev accum and redo this special
				m_iLastTokenLen = m_iAccum;
				m_sAccum[m_iAccum] = '\0';
				m_pCur--;
				m_pTokenEnd--;
			}

			m_iAccum = 0;
			if ( !BlendAdjust ( pCur ) )
				continue;
			if ( m_bBlended )
				return GetBlendedVariant();
			return m_sAccum;
		}

		// just accumulate
		assert ( iCode>0 );
		if ( m_iAccum<SPH_MAX_WORD_LEN )
		{
			if ( m_iAccum==0 )
				m_pTokenStart = pCur;

			// tricky bit
			// heading modifiers must not (!) affected blended status
			// eg. we want stuff like '=-' (w/o apostrophes) thrown away when pure_blend is on
			if_const (!( IS_QUERY && !m_iAccum && sphIsModifier(iCode) ) )
				m_bNonBlended = m_bNonBlended || bNoBlend;
			m_sAccum[m_iAccum++] = (BYTE)iCode;
		}
	}
}


template < bool IS_QUERY >
ISphTokenizer * CSphTokenizer_SBCS<IS_QUERY>::Clone ( ESphTokenizerClone eMode ) const
{
	CSphTokenizerBase * pClone;
	if ( eMode!=SPH_CLONE_INDEX )
		pClone = new CSphTokenizer_SBCS<true>();
	else
		pClone = new CSphTokenizer_SBCS<false>();
	pClone->CloneBase ( this, eMode );
	return pClone;
}

/////////////////////////////////////////////////////////////////////////////

template < bool IS_QUERY >
CSphTokenizer_UTF8<IS_QUERY>::CSphTokenizer_UTF8 ()
{
	CSphString sTmp;
	SetCaseFolding ( SPHINX_DEFAULT_UTF8_TABLE, sTmp );
	m_bHasBlend = false;
}


template < bool IS_QUERY >
void CSphTokenizer_UTF8<IS_QUERY>::SetBuffer ( const BYTE * sBuffer, int iLength )
{
	// check that old one is over and that new length is sane
	assert ( iLength>=0 );

	// set buffer
	m_pBuffer = sBuffer;
	m_pBufferMax = sBuffer + iLength;
	m_pCur = sBuffer;
	m_pTokenStart = m_pTokenEnd = NULL;
	m_pBlendStart = m_pBlendEnd = NULL;

	m_iOvershortCount = 0;
	m_bBoundary = m_bTokenBoundary = false;
}


template < bool IS_QUERY >
BYTE * CSphTokenizer_UTF8<IS_QUERY>::GetToken ()
{
	m_bWasSpecial = false;
	m_bBlended = false;
	m_iOvershortCount = 0;
	m_bTokenBoundary = false;

	if ( m_dSynonyms.GetLength() )
		return GetTokenSyn ( IS_QUERY );

	return m_bHasBlend
		? DoGetToken<IS_QUERY,true>()
		: DoGetToken<IS_QUERY,false>();
}


template < bool IS_QUERY, bool IS_BLEND >
BYTE * CSphTokenizer_UTF8_Base::DoGetToken ()
{
	// return pending blending variants
	if_const ( IS_BLEND )
	{
		BYTE * pVar = GetBlendedVariant ();
		if ( pVar )
			return pVar;
		m_bBlendedPart = ( m_pBlendEnd!=NULL );
	}

	// in query mode, lets capture (soft-whitespace hard-whitespace) sequences and adjust overshort counter
	// sample queries would be (one NEAR $$$) or (one | $$$ two) where $ is not a valid character
	bool bGotNonToken = ( !IS_QUERY || m_bPhrase ); // only do this in query mode, never in indexing mode, never within phrases
	bool bGotSoft = false; // hey Beavis he said soft huh huhhuh

	for ( ;; )
	{
		// get next codepoint
		const BYTE * pCur = m_pCur; // to redo special char, if there's a token already

		int iCodePoint;
		int iCode;
		if ( pCur<m_pBufferMax && *pCur<128 )
		{
			iCodePoint = *m_pCur++;
			iCode = m_tLC.m_pChunk[0][iCodePoint];
		} else
		{
			iCodePoint = GetCodepoint(); // advances m_pCur
			iCode = m_tLC.ToLower ( iCodePoint );
		}

		// handle escaping
		bool bWasEscaped = ( IS_QUERY && iCodePoint=='\\' ); // whether current codepoint was escaped
		if ( bWasEscaped )
		{
			iCodePoint = GetCodepoint();
			iCode = m_tLC.ToLower ( iCodePoint );
			if ( !Special2Simple ( iCode ) )
				iCode = 0;
		}

		// handle eof
		if ( iCode<0 )
		{
			// skip trailing short word
			FlushAccum ();
			if ( m_iLastTokenLen<m_tSettings.m_iMinWordLen )
			{
				if ( !m_bShortTokenFilter || !ShortTokenFilter ( m_sAccum, m_iLastTokenLen ) )
				{
					if ( m_iLastTokenLen )
						m_iOvershortCount++;
					m_iLastTokenLen = 0;
					if_const ( IS_BLEND )
						BlendAdjust ( pCur );
					return NULL;
				}
			}

			// keep token end here as BlendAdjust might change m_pCur
			m_pTokenEnd = m_pCur;

			// return trailing word
			if_const ( IS_BLEND && !BlendAdjust ( pCur ) )
				return NULL;
			if_const ( IS_BLEND && m_bBlended )
				return GetBlendedVariant();
			return m_sAccum;
		}

		// handle all the flags..
		if_const ( IS_QUERY )
			iCode = CodepointArbitrationQ ( iCode, bWasEscaped, *m_pCur );
		else if ( m_bDetectSentences )
			iCode = CodepointArbitrationI ( iCode );

		// handle ignored chars
		if ( iCode & FLAG_CODEPOINT_IGNORE )
			continue;

		// handle blended characters
		if_const ( IS_BLEND && ( iCode & FLAG_CODEPOINT_BLEND ) )
		{
			if ( m_pBlendEnd )
				iCode = 0;
			else
			{
				m_bBlended = true;
				m_pBlendStart = m_iAccum ? m_pTokenStart : pCur;
			}
		}

		// handle soft-whitespace-only tokens
		if ( !bGotNonToken && !m_iAccum )
		{
			if ( !bGotSoft )
			{
				// detect opening soft whitespace
				if ( ( iCode==0 && !( iCode & MASK_FLAGS ) && !IsWhitespace ( iCodePoint ) )
					|| ( ( iCode & FLAG_CODEPOINT_BLEND ) && !m_iAccum ) )
				{
					bGotSoft = true;
				}
			} else
			{
				// detect closing hard whitespace or special
				// (if there was anything meaningful in the meantime, we must never get past the outer if!)
				if ( IsWhitespace ( iCodePoint ) || ( iCode & FLAG_CODEPOINT_SPECIAL ) )
				{
					m_iOvershortCount++;
					bGotNonToken = true;
				}
			}
		}

		// handle whitespace and boundary
		if ( m_bBoundary && ( iCode==0 ) )
		{
			m_bTokenBoundary = true;
			m_iBoundaryOffset = pCur - m_pBuffer - 1;
		}
		m_bBoundary = ( iCode & FLAG_CODEPOINT_BOUNDARY )!=0;

		if ( iCode==0 || m_bBoundary )
		{
			FlushAccum ();
			if_const ( IS_BLEND && !BlendAdjust ( pCur ) )
				continue;

			if ( m_iLastTokenLen<m_tSettings.m_iMinWordLen
				&& !( m_bShortTokenFilter && ShortTokenFilter ( m_sAccum, m_iLastTokenLen ) ) )
			{
				if ( m_iLastTokenLen )
					m_iOvershortCount++;
				continue;
			} else
			{
				m_pTokenEnd = pCur;
				if_const ( IS_BLEND && m_bBlended )
					return GetBlendedVariant();
				return m_sAccum;
			}
		}

		// handle specials
		if ( iCode & FLAG_CODEPOINT_SPECIAL )
		{
			// skip short words preceding specials
			if ( m_iAccum<m_tSettings.m_iMinWordLen )
			{
				m_sAccum[m_iAccum] = '\0';

				if ( !m_bShortTokenFilter || !ShortTokenFilter ( m_sAccum, m_iAccum ) )
				{
					if ( m_iAccum )
						m_iOvershortCount++;

					FlushAccum ();
				}
			}

			if ( m_iAccum==0 )
			{
				m_bNonBlended = m_bNonBlended || ( !( iCode & FLAG_CODEPOINT_BLEND ) && !( iCode & FLAG_CODEPOINT_SPECIAL ) );
				m_bWasSpecial = !( iCode & FLAG_CODEPOINT_NGRAM );
				m_pTokenStart = pCur;
				m_pTokenEnd = m_pCur;
				AccumCodepoint ( iCode & MASK_CODEPOINT ); // handle special as a standalone token
			} else
			{
				m_pCur = pCur; // we need to flush current accum and then redo special char again
				m_pTokenEnd = pCur;
			}

			FlushAccum ();
			if_const ( IS_BLEND )
			{
				if ( !BlendAdjust ( pCur ) )
					continue;
				if ( m_bBlended )
					return GetBlendedVariant();
			}
			return m_sAccum;
		}

		if ( m_iAccum==0 )
			m_pTokenStart = pCur;

		// tricky bit
		// heading modifiers must not (!) affected blended status
		// eg. we want stuff like '=-' (w/o apostrophes) thrown away when pure_blend is on
		if_const ( IS_BLEND )
			if_const (!( IS_QUERY && !m_iAccum && sphIsModifier ( iCode & MASK_CODEPOINT ) ) )
				m_bNonBlended = m_bNonBlended || !( iCode & FLAG_CODEPOINT_BLEND );

		// just accumulate
		// manual inlining of utf8 encoder gives us a few extra percent
		// which is important here, this is a hotspot
		if ( m_iAccum<SPH_MAX_WORD_LEN && ( m_pAccum-m_sAccum+SPH_MAX_UTF8_BYTES<=(int)sizeof(m_sAccum) ) )
		{
			iCode &= MASK_CODEPOINT;
			m_iAccum++;
			SPH_UTF8_ENCODE ( m_pAccum, iCode );
		}
	}
}


void CSphTokenizer_UTF8_Base::FlushAccum ()
{
	assert ( m_pAccum-m_sAccum < (int)sizeof(m_sAccum) );
	m_iLastTokenLen = m_iAccum;
	*m_pAccum = 0;
	m_iAccum = 0;
	m_pAccum = m_sAccum;
}


template < bool IS_QUERY >
ISphTokenizer * CSphTokenizer_UTF8<IS_QUERY>::Clone ( ESphTokenizerClone eMode ) const
{
	CSphTokenizerBase * pClone;
	if ( eMode!=SPH_CLONE_INDEX )
		pClone = new CSphTokenizer_UTF8<true>();
	else
		pClone = new CSphTokenizer_UTF8<false>();
	pClone->CloneBase ( this, eMode );
	return pClone;
}


template < bool IS_QUERY >
int CSphTokenizer_UTF8<IS_QUERY>::GetCodepointLength ( int iCode ) const
{
	if ( iCode<128 )
		return 1;

	int iBytes = 0;
	while ( iCode & 0x80 )
	{
		iBytes++;
		iCode <<= 1;
	}

	assert ( iBytes>=2 && iBytes<=4 );
	return iBytes;
}






//===========================================================================================
/////////////////////////////////////////////////////////////////////////////

template < bool IS_QUERY >
CSphTokenizer_UTF8Chinese<IS_QUERY>::CSphTokenizer_UTF8Chinese ()
{
        CSphString sTmp;
        SetCaseFolding ( SPHINX_DEFAULT_UTF8_TABLE, sTmp );
        m_bHasBlend = false;
}


template < bool IS_QUERY >
void CSphTokenizer_UTF8Chinese<IS_QUERY>::SetBuffer ( const BYTE * sBuffer, int iLength )
{
        // check that old one is over and that new length is sane
        assert ( iLength>=0 );

        // set buffer
        m_pBuffer = sBuffer;
        m_pBufferMax = sBuffer + iLength;
        m_pCur = sBuffer;
        m_pTokenStart = m_pTokenEnd = NULL;
        m_pBlendStart = m_pBlendEnd = NULL;

        m_iOvershortCount = 0;
        m_bBoundary = m_bTokenBoundary = false;
}

template < bool IS_QUERY >
int CSphTokenizer_UTF8Chinese<IS_QUERY>::getBestWordLength(Darts::DoubleArray::key_type *pText, size_t iTextLength) {
    Darts::DoubleArray::result_pair_type  dResultPair_A[64];
    Darts::DoubleArray::result_pair_type  dResultPair_B[64];
    Darts::DoubleArray::result_pair_type  dResultPair_C[64];

    size_t iNum_A, iNum_B, iNum_C; //A B C stand for the first, sencod, third word in a chunk
    register size_t iCur_A, iCur_B, iCur_C;
    Darts::DoubleArray::key_type *pText_A, *pText_B, *pText_C;
    size_t iTextLength_A, iTextLength_B, iTextLength_C;
    
    //we use a float 10-element-array to store a chunk
    //0-word count
    //1-3 byte length of each word
    //4-6 frequence of each word
    //7-9 character count of each word

    float dChunks[64][10]={{0}};
    size_t iChunkCur=0;
    size_t iChunkCount;

    float dChunk_A[10]={0};
    float dChunk_B[10]={0};

    size_t iByteCount;
    register size_t iByteCur;
    size_t iChunkSize=sizeof(float)*10;        

    size_t dClip[64];//temporary cursor data when filtering
    size_t dBest[64];//the best result. There may be more than 1 chunk at last, so we use array
    register size_t iClipCur=0;
    size_t iClipSize=0;
    register size_t iBestCur=0;
    size_t iResultSize = sizeof(int)*64;

    float dLengths[64]={0}; //chunks' lengths
    float dWordCounts[64]={0}; //for chunks' average lengths, but we use word count when filtering with this rule
    float dVariances[64]={0}; //chunks' variances
    float dSDMFs[64]={0}; //chunks' sum of degree of morphemic freedom of one-character words

    float iMaxChunkLength=0;
    float fMinWordCount;//used to filtering with max average length, which also means having the minimal word count;
    float fMinVariance;
        float fAvgLength;
    int iTemp; //mostly for filtering with min variance
    float fTemp; ////mostly for filtering with min variance
    float fMaxSDMF=0;

    pText_A=pText;
    iTextLength_A=iTextLength;
    //get all the chunks
    iNum_A = m_tDa.commonPrefixSearch(pText_A, dResultPair_A, 64, iTextLength_A);
    for(iCur_A=0;iCur_A<iNum_A;iCur_A++) {
        dChunk_A[0] = 1;
        dChunk_A[1] = dResultPair_A[iCur_A].length;
        dChunk_A[4] = dResultPair_A[iCur_A].value;
        //character count
        iByteCount=0;
        for(iByteCur=0;iByteCur<dChunk_A[1];iByteCur++) {
                        if(UTF8CharLenTable[(unsigned char)(*(pText_A+iByteCur))]<0x08) {
                iByteCount++;
            }
        }
        dChunk_A[7] = iByteCount;

        pText_B = pText_A + dResultPair_A[iCur_A].length;
        iTextLength_B = iTextLength_A - dResultPair_A[iCur_A].length;

        iNum_B = m_tDa.commonPrefixSearch(pText_B, dResultPair_B, 64, iTextLength_B);
        if(iNum_B) {        
            for(iCur_B=0;iCur_B<iNum_B;iCur_B++) {
                memcpy(dChunk_B, dChunk_A, iChunkSize);//initialize chunk B
                dChunk_B[0] = 2;            
                dChunk_B[2] = dResultPair_B[iCur_B].length;
                dChunk_B[5] = dResultPair_B[iCur_B].value;
                ///character count
                iByteCount=0;
                for(iByteCur=0;iByteCur<dChunk_B[2];iByteCur++) {
                                if(UTF8CharLenTable[(unsigned char)(*(pText_B+iByteCur))]<0x08) {
                        iByteCount++;
                    }
                }
                dChunk_B[8] = iByteCount;

                pText_C = pText_B + dResultPair_B[iCur_B].length;
                iTextLength_C = iTextLength_B - dResultPair_B[iCur_B].length;
                iNum_C = m_tDa.commonPrefixSearch(pText_C , dResultPair_C, 64, iTextLength_C);
                if(iNum_C) {
                    for(iCur_C=0;iCur_C<iNum_C;iCur_C++) {
                        memcpy(dChunks[iChunkCur], dChunk_B, iChunkSize);

                        dChunks[iChunkCur][0] = 3;
                        dChunks[iChunkCur][3] = dResultPair_C[iCur_C].length;
                        dChunks[iChunkCur][6] = dResultPair_C[iCur_C].value;
                        //character count
                        iByteCount=0;
                        for(iByteCur=0;iByteCur<dChunks[iChunkCur][3];iByteCur++) {
                                                if(UTF8CharLenTable[(unsigned char)(*(pText_C+iByteCur))]<0x08) {
                                iByteCount++;
                            }
                        }
                        dChunks[iChunkCur][9] = iByteCount;

                        iChunkCur++;
                    }
                } else {
                    memcpy(dChunks[iChunkCur], dChunk_B, iChunkSize);
                    iChunkCur++;
                }//if(iNum_C)
            }
        } else {
            memcpy(dChunks[iChunkCur], dChunk_A, iChunkSize);
            iChunkCur++;
        }//if(iNum_B)
    }//the main loop of obtaining chunks

    iChunkCount=iChunkCur;//just remember the chunks count

    //filter with max length;
    for(iChunkCur=0;iChunkCur<iChunkCount;iChunkCur++) {
        dLengths[iChunkCur] = dChunks[iChunkCur][7] + dChunks[iChunkCur][8] + dChunks[iChunkCur][9];
        iMaxChunkLength = iMaxChunkLength > dLengths[iChunkCur] ? iMaxChunkLength : dLengths[iChunkCur];
    }
    for(iChunkCur=0;iChunkCur<iChunkCount;iChunkCur++) {
        if(iMaxChunkLength == dLengths[iChunkCur]) {
            dBest[iBestCur++]=iChunkCur;
        }
    }
    if(iBestCur<2) {
        return (int)dChunks[dBest[0]][1];
    }

    //filter with average word legnth

    memcpy(dClip, dBest, iResultSize);
    iClipSize=iBestCur;

        //initialize the first one
        dWordCounts[dClip[0]] = dChunks[dClip[0]][0];
        fMinWordCount=dChunks[dClip[0]][0];

    for(iClipCur=1;iClipCur<iClipSize;iClipCur++) {//notice that iClipCur starts from 1 (not 0)
        dWordCounts[dClip[iClipCur]] = dChunks[dClip[iClipCur]][0];
        fMinWordCount = fMinWordCount < dWordCounts[dClip[iClipCur]] ? fMinWordCount : dWordCounts[dClip[iClipCur]];
    }
    iBestCur=0;
    for(iClipCur=0;iClipCur<iClipSize;iClipCur++) {
        if(fMinWordCount == dWordCounts[dClip[iClipCur]]) {
            dBest[iBestCur++]=dClip[iClipCur];
        }
    }
    if(iBestCur<2) {
        return (int)dChunks[dBest[0]][1];
    }

    //filter with min variance
    
    memcpy(dClip, dBest, iResultSize);
    iClipSize=iBestCur;

        //initialize the first one
        fAvgLength = (dChunks[dClip[0]][7]+dChunks[dClip[0]][8]+dChunks[dClip[0]][9])/dChunks[dClip[0]][0];
    for(iTemp=0;iTemp<dChunks[dClip[0]][0];iTemp++) {
        fTemp= fAvgLength - dChunks[dClip[0]][iTemp+7];
        dVariances[dClip[0]] += fTemp*fTemp;
    }
    dVariances[dClip[0]] = sqrt(dVariances[dClip[0]]/dChunks[dClip[0]][0]);
    fMinVariance = dVariances[dClip[0]];

    for(iClipCur=1;iClipCur<iClipSize;iClipCur++) {//notice that iClipCur starts from 1, not 0
                fAvgLength = (dChunks[dClip[iClipCur]][7]+dChunks[dClip[iClipCur]][8]+dChunks[dClip[iClipCur]][9])/dChunks[dClip[iClipCur]][0];
        for(iTemp=0;iTemp<dChunks[dClip[iClipCur]][0];iTemp++) {
            fTemp = fAvgLength - dChunks[dClip[iClipCur]][iTemp+7];
            dVariances[dClip[iClipCur]] += fTemp*fTemp;
        }
        dVariances[dClip[iClipCur]] = sqrt(dVariances[dClip[iClipCur]]/dChunks[dClip[iClipCur]][0]);
        fMinVariance = fMinVariance < dVariances[dClip[iClipCur]] ? fMinVariance : dVariances[dClip[iClipCur]];
    }
    iBestCur=0;
    for(iClipCur=0;iClipCur<iClipSize;iClipCur++) {
        if(fMinVariance == dVariances[dClip[iClipCur]]) {
            dBest[iBestCur++]=dClip[iClipCur];
        }
    }
    if(iBestCur<2) {
        return (int)dChunks[dBest[0]][1];
    }

    //filter with max SDMF(sum of degree of morphemic freedom of one-character words)

    memcpy(dClip, dBest, iResultSize);
    iClipSize=iBestCur;

    for(iClipCur=0;iClipCur<iClipSize;iClipCur++) {
        for(iTemp=0;iTemp<dChunks[dClip[iClipCur]][0];iTemp++) {
            if(dChunks[dClip[iClipCur]][iTemp+7]==1) {
                dSDMFs[dClip[iClipCur]]+=log(dChunks[dClip[iClipCur]][iTemp+4]);
            }
        }
        fMaxSDMF = fMaxSDMF > dSDMFs[dClip[iClipCur]] ? fMaxSDMF : dSDMFs[dClip[iClipCur]];
    }
    iBestCur=0;
    for(iClipCur=0;iClipCur<iClipSize;iClipCur++) {
        if(fMaxSDMF == dSDMFs[dClip[iClipCur]]) {
            dBest[iBestCur++]=dClip[iClipCur];
        }
    }
    //if(iBestCur<2) {
    return (int)dChunks[dBest[0]][1];
    //}    
}

template < bool IS_QUERY >
bool CSphTokenizer_UTF8Chinese<IS_QUERY>::SetChineseDictionary ( const char * sConfig, CSphString & sError )
{
        //initialize Chinese dictionary
        if(!g_pChineseDictionary) {
#if USE_WINDOWS 
                struct _stat sb;
                if (_stat (sConfig, &sb) == -1) {
#else
                struct stat sb;
                if (stat (sConfig, &sb) == -1) {
#endif
                        sError="Can not analyze Chinese dictionary file!";
                        return false;
                }

#if USE_WINDOWS
                HANDLE mmf = CreateFile (sConfig, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

                if (mmf == INVALID_HANDLE_VALUE) {
                        sError = "Can not open Chinese dictionary!";
                        return false;
                }

                HANDLE mmfv = CreateFileMapping (mmf, NULL, PAGE_READONLY, 0, 0, NULL);
                if (mmfv == INVALID_HANDLE_VALUE) {
                        CloseHandle (mmf);
                        sError = "Can not map Chinese dictionary to memory!";
                        return false;
                }

                g_pChineseDictionary = MapViewOfFile (mmfv, FILE_MAP_READ, 0, 0, sb.st_size);

                // The handles can be safely closed
                CloseHandle (mmf);
                CloseHandle (mmfv);
#else
                int fd;
                if((fd = open(sConfig, O_RDONLY)) == -1) {
                        sError = "Can not open Chinese dictionary!";
                        return false;
                }

                if(!(g_pChineseDictionary = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, fd, 0))) {
                        sError = "Can not map Chinese dictionary to memory!";
                        return false;
                }
#endif
        }

        m_tDa.set_array(g_pChineseDictionary);
        return true;
}


template < bool IS_QUERY >
BYTE * CSphTokenizer_UTF8Chinese<IS_QUERY>::GetToken ()
{
        m_bWasSpecial = false;
        m_bBlended = false;
        m_iOvershortCount = 0;
        m_bTokenBoundary = false;

        if ( m_dSynonyms.GetLength() )
                return GetTokenSyn ( IS_QUERY );
        bool IS_BLEND = m_bHasBlend;
        
        // return pending blending variants
        if_const ( IS_BLEND )
        {
                BYTE * pVar = GetBlendedVariant ();
                if ( pVar )
                        return pVar;
                m_bBlendedPart = ( m_pBlendEnd!=NULL );
        }

        // in query mode, lets capture (soft-whitespace hard-whitespace) sequences and adjust overshort counter
        // sample queries would be (one NEAR $$$) or (one | $$$ two) where $ is not a valid character
        bool bGotNonToken = ( !IS_QUERY || m_bPhrase ); // only do this in query mode, never in indexing mode, never within phrases
        bool bGotSoft = false; // hey Beavis he said soft huh huhhuh

        size_t iWordLength=0;
        int iNum;

        while(iWordLength==0){
                m_pText=(Darts::DoubleArray::key_type *)m_pCur;
                iNum = m_tDa.commonPrefixSearch(m_pText, m_pResultPair, 256, m_pBufferMax-m_pCur);

                if( iNum == 0) {

//original utf8 handler, it is borrowed here to process unmatched characters
//mostly for the latin characters I guess
//start=====================================================================        
        
        for ( ;; )
        {
                // get next codepoint
                const BYTE * pCur = m_pCur; // to redo special char, if there's a token already

                int iCodePoint;
                int iCode;
                if ( pCur<m_pBufferMax && *pCur<128 )
                {
                        iCodePoint = *m_pCur++;
                        iCode = m_tLC.m_pChunk[0][iCodePoint];
                } else
                {
                        iCodePoint = GetCodepoint(); // advances m_pCur
                        iCode = m_tLC.ToLower ( iCodePoint );
                }

                // handle escaping
                bool bWasEscaped = ( IS_QUERY && iCodePoint=='\\' ); // whether current codepoint was escaped
                if ( bWasEscaped )
                {
                        iCodePoint = GetCodepoint();
                        iCode = m_tLC.ToLower ( iCodePoint );
                        if ( !Special2Simple ( iCode ) )
                                iCode = 0;
                }

                // handle eof
                if ( iCode<0 )
                {
                        // skip trailing short word
                        FlushAccum ();
                        if ( m_iLastTokenLen<m_tSettings.m_iMinWordLen )
                        {
                                if ( !m_bShortTokenFilter || !ShortTokenFilter ( m_sAccum, m_iLastTokenLen ) )
                                {
                                        if ( m_iLastTokenLen )
                                                m_iOvershortCount++;
                                        m_iLastTokenLen = 0;
                                        if_const ( IS_BLEND )
                                                BlendAdjust ( pCur );
                                        return NULL;
                                }
                        }

                        // keep token end here as BlendAdjust might change m_pCur
                        m_pTokenEnd = m_pCur;

                        // return trailing word
                        if_const ( IS_BLEND && !BlendAdjust ( pCur ) )
                                return NULL;
                        if_const ( IS_BLEND && m_bBlended )
                                return GetBlendedVariant();
                        return m_sAccum;
                }

                // handle all the flags..
                if_const ( IS_QUERY )
                        iCode = CodepointArbitrationQ ( iCode, bWasEscaped, *m_pCur );
                else if ( m_bDetectSentences )
                        iCode = CodepointArbitrationI ( iCode );

                // handle ignored chars
                if ( iCode & FLAG_CODEPOINT_IGNORE )
                        continue;

                // handle blended characters
                if_const ( IS_BLEND && ( iCode & FLAG_CODEPOINT_BLEND ) )
                {
                        if ( m_pBlendEnd )
                                iCode = 0;
                        else
                        {
                                m_bBlended = true;
                                m_pBlendStart = m_iAccum ? m_pTokenStart : pCur;
                        }
                }

                // handle soft-whitespace-only tokens
                if ( !bGotNonToken && !m_iAccum )
                {
                        if ( !bGotSoft )
                        {
                                // detect opening soft whitespace
                                if ( ( iCode==0 && !( iCode & MASK_FLAGS ) && !IsWhitespace ( iCodePoint ) )
                                        || ( ( iCode & FLAG_CODEPOINT_BLEND ) && !m_iAccum ) )
                                {
                                        bGotSoft = true;
                                }
                        } else
                        {
                                // detect closing hard whitespace or special
                                // (if there was anything meaningful in the meantime, we must never get past the outer if!)
                                if ( IsWhitespace ( iCodePoint ) || ( iCode & FLAG_CODEPOINT_SPECIAL ) )
                                {
                                        m_iOvershortCount++;
                                        bGotNonToken = true;
                                }
                        }
                }

                // handle whitespace and boundary
                if ( m_bBoundary && ( iCode==0 ) )
                {
                        m_bTokenBoundary = true;
                        m_iBoundaryOffset = pCur - m_pBuffer - 1;
                }
                m_bBoundary = ( iCode & FLAG_CODEPOINT_BOUNDARY )!=0;

                if ( iCode==0 || m_bBoundary )
                {
                        FlushAccum ();
                        if_const ( IS_BLEND && !BlendAdjust ( pCur ) )
                                continue;

                        if ( m_iLastTokenLen<m_tSettings.m_iMinWordLen
                                && !( m_bShortTokenFilter && ShortTokenFilter ( m_sAccum, m_iLastTokenLen ) ) )
                        {
                                if ( m_iLastTokenLen )
                                        m_iOvershortCount++;
                                //continue;
                                break;
                        } else
                        {
                                m_pTokenEnd = pCur;
                                if(m_tDa.commonPrefixSearch((Darts::DoubleArray::key_type *)(pCur), m_pResultPair, 256, m_pBufferMax-m_pCur)>0) {
                                        m_pCur=pCur;
                                }

                                if_const ( IS_BLEND && m_bBlended )
                                        return GetBlendedVariant();
                                return m_sAccum;
                        }
                }

                // handle specials
                if ( iCode & FLAG_CODEPOINT_SPECIAL )
                {
                        // skip short words preceding specials
                        if ( m_iAccum<m_tSettings.m_iMinWordLen )
                        {
                                m_sAccum[m_iAccum] = '\0';

                                if ( !m_bShortTokenFilter || !ShortTokenFilter ( m_sAccum, m_iAccum ) )
                                {
                                        if ( m_iAccum )
                                                m_iOvershortCount++;

                                        FlushAccum ();
                                }
                        }

                        if ( m_iAccum==0 )
                        {
                                m_bNonBlended = m_bNonBlended || ( !( iCode & FLAG_CODEPOINT_BLEND ) && !( iCode & FLAG_CODEPOINT_SPECIAL ) );
                                m_bWasSpecial = !( iCode & FLAG_CODEPOINT_NGRAM );
                                m_pTokenStart = pCur;
                                m_pTokenEnd = m_pCur;
                                AccumCodepoint ( iCode & MASK_CODEPOINT ); // handle special as a standalone token
                        } else
                        {
                                m_pCur = pCur; // we need to flush current accum and then redo special char again
                                m_pTokenEnd = pCur;
                        }

                        FlushAccum ();
                        if_const ( IS_BLEND )
                        {
                                if ( !BlendAdjust ( pCur ) )
                                        continue;
                                if ( m_bBlended )
                                        return GetBlendedVariant();
                        }
                        return m_sAccum;
                }

                if ( m_iAccum==0 )
                        m_pTokenStart = pCur;

                // tricky bit
                // heading modifiers must not (!) affected blended status
                // eg. we want stuff like '=-' (w/o apostrophes) thrown away when pure_blend is on
                if_const ( IS_BLEND )
                        if_const (!( IS_QUERY && !m_iAccum && sphIsModifier ( iCode & MASK_CODEPOINT ) ) )
                                m_bNonBlended = m_bNonBlended || !( iCode & FLAG_CODEPOINT_BLEND );

                // just accumulate
                // manual inlining of utf8 encoder gives us a few extra percent
                // which is important here, this is a hotspot
                if ( m_iAccum<SPH_MAX_WORD_LEN && ( m_pAccum-m_sAccum+SPH_MAX_UTF8_BYTES<=(int)sizeof(m_sAccum) ) )
                {
                        iCode &= MASK_CODEPOINT;
                        m_iAccum++;
                        SPH_UTF8_ENCODE ( m_pAccum, iCode );
                }
        }
        
//end=====================================================================
                } else if(iNum==1) {
                        iWordLength=m_pResultPair[0].length;
                } else if(iNum > 1) { 
                        //we use MMSEG algorithm to get the best next word length
                        iWordLength=getBestWordLength(m_pText, m_pBufferMax-m_pCur);
                }//end of if
        }//end of while

        if(iWordLength==0)
                return NULL;

        assert ( iWordLength < (int)sizeof(m_sAccum) );
        memcpy(m_sAccum, m_pText, iWordLength);
        m_sAccum[iWordLength]='\0';

        m_pTokenStart = m_pCur;
        m_pCur += iWordLength;
        m_pTokenEnd = m_pCur;

        return m_sAccum;


}


template < bool IS_QUERY >
void CSphTokenizer_UTF8Chinese<IS_QUERY>::FlushAccum ()
{
        assert ( m_pAccum-m_sAccum < (int)sizeof(m_sAccum) );
        m_iLastTokenLen = m_iAccum;
        *m_pAccum = 0;
        m_iAccum = 0;
        m_pAccum = m_sAccum;
}


template < bool IS_QUERY >
ISphTokenizer * CSphTokenizer_UTF8Chinese<IS_QUERY>::Clone ( ESphTokenizerClone eMode ) const
{
        //CSphTokenizer_UTF8Chinese * pClone;
        if ( eMode!=SPH_CLONE_INDEX ) {
                CSphTokenizer_UTF8Chinese<true> *pClone = new CSphTokenizer_UTF8Chinese<true>();
                pClone->CloneBase ( this, eMode );
                pClone->m_tDa.set_array(g_pChineseDictionary);
                return pClone;

        } else {
                CSphTokenizer_UTF8Chinese<false> *pClone = new CSphTokenizer_UTF8Chinese<false>();
                pClone->CloneBase ( this, eMode );
                pClone->m_tDa.set_array(g_pChineseDictionary);
                return pClone;
        }
}


template < bool IS_QUERY >
int CSphTokenizer_UTF8Chinese<IS_QUERY>::GetCodepointLength ( int iCode ) const
{
        if ( iCode<128 )
                return 1;

        int iBytes = 0;
        while ( iCode & 0x80 )
        {
                iBytes++;
                iCode <<= 1;
        }

        assert ( iBytes>=2 && iBytes<=4 );
        return iBytes;
}
//===========================================================================================






























/////////////////////////////////////////////////////////////////////////////

template < bool IS_QUERY >
bool CSphTokenizer_UTF8Ngram<IS_QUERY>::SetNgramChars ( const char * sConfig, CSphString & sError )
{
	assert ( this->m_eMode!=SPH_CLONE_QUERY_LIGHTWEIGHT );
	CSphVector<CSphRemapRange> dRemaps;
	CSphCharsetDefinitionParser tParser;
	if ( !tParser.Parse ( sConfig, dRemaps ) )
	{
		sError = tParser.GetLastError();
		return false;
	}

	// gcc braindamage requires this
	this->m_tLC.AddRemaps ( dRemaps, FLAG_CODEPOINT_NGRAM | FLAG_CODEPOINT_SPECIAL ); // !COMMIT support other n-gram lengths than 1
	m_sNgramCharsStr = sConfig;
	return true;
}


template < bool IS_QUERY >
void CSphTokenizer_UTF8Ngram<IS_QUERY>::SetNgramLen ( int iLen )
{
	assert ( this->m_eMode!=SPH_CLONE_QUERY_LIGHTWEIGHT );
	assert ( iLen>0 );
	m_iNgramLen = iLen;
}


template < bool IS_QUERY >
BYTE * CSphTokenizer_UTF8Ngram<IS_QUERY>::GetToken ()
{
	// !COMMIT support other n-gram lengths than 1
	assert ( m_iNgramLen==1 );
	return CSphTokenizer_UTF8<IS_QUERY>::GetToken ();
}

//////////////////////////////////////////////////////////////////////////

CSphMultiformTokenizer::CSphMultiformTokenizer ( ISphTokenizer * pTokenizer, const CSphMultiformContainer * pContainer )
	: CSphTokenFilter ( pTokenizer )
	, m_pMultiWordforms ( pContainer )
	, m_iStart	( 0 )
	, m_bBuildMultiform	( false )
{
	assert ( pTokenizer && pContainer );
	m_dStoredTokens.Reserve ( pContainer->m_iMaxTokens + 6 ); // max form tokens + some blended tokens
	m_sTokenizedMultiform[0] = '\0';
}


CSphMultiformTokenizer::~CSphMultiformTokenizer ()
{
	SafeDelete ( m_pTokenizer );
}


BYTE * CSphMultiformTokenizer::GetToken ()
{
	m_sTokenizedMultiform[0] = '\0';
	m_iStart++;

	if ( m_iStart>=m_dStoredTokens.GetLength() )
	{
		m_iStart = 0;
		m_dStoredTokens.Resize ( 0 );
		const BYTE * pToken = m_pTokenizer->GetToken();
		if ( !pToken )
			return NULL;

		FillStoredTokenInfo ( m_dStoredTokens.Add(), pToken, m_pTokenizer );
		while ( m_dStoredTokens.Last().m_bBlended || m_dStoredTokens.Last().m_bBlendedPart )
		{
			pToken = m_pTokenizer->GetToken ();
			if ( !pToken )
				break;

			FillStoredTokenInfo ( m_dStoredTokens.Add(), pToken, m_pTokenizer );
		}
	}

	CSphMultiforms ** pWordforms = NULL;
	int iTokensGot = 1;
	bool bBlended = false;

	// check multi-form
	// only blended parts checked for multi-form with blended
	// in case ALL blended parts got transformed primary blended got replaced by normal form
	// otherwise blended tokens provided as is
	if ( m_dStoredTokens[m_iStart].m_bBlended || m_dStoredTokens[m_iStart].m_bBlendedPart )
	{
		if ( m_dStoredTokens[m_iStart].m_bBlended && m_iStart+1<m_dStoredTokens.GetLength() && m_dStoredTokens[m_iStart+1].m_bBlendedPart )
		{
			pWordforms = m_pMultiWordforms->m_Hash ( (const char *)m_dStoredTokens[m_iStart+1].m_sToken );
			if ( pWordforms )
			{
				bBlended = true;
				for ( int i=m_iStart+2; i<m_dStoredTokens.GetLength(); i++ )
				{
					// break out on blended over or got completely different blended
					if ( m_dStoredTokens[i].m_bBlended || !m_dStoredTokens[i].m_bBlendedPart )
						break;

					iTokensGot++;
				}
			}
		}
	} else
	{
		pWordforms = m_pMultiWordforms->m_Hash ( (const char *)m_dStoredTokens[m_iStart].m_sToken );
		if ( pWordforms )
		{
			int iTokensNeed = (*pWordforms)->m_iMaxTokens + 1;
			int iCur = m_iStart;
			bool bGotBlended = false;

			// collect up ahead to multi-form tokens or all blended tokens
			while ( iTokensGot<iTokensNeed || bGotBlended )
			{
				iCur++;
				if ( iCur>=m_dStoredTokens.GetLength() )
				{
					// fetch next token
					const BYTE* pToken = m_pTokenizer->GetToken ();
					if ( !pToken )
						break;

					FillStoredTokenInfo ( m_dStoredTokens.Add(), pToken, m_pTokenizer );
				}

				bool bCurBleneded = ( m_dStoredTokens[iCur].m_bBlended || m_dStoredTokens[iCur].m_bBlendedPart );
				if ( bGotBlended && !bCurBleneded )
					break;

				bGotBlended = bCurBleneded;
				// count only regular tokens; can not fold mixed (regular+blended) tokens to form
				iTokensGot += ( bGotBlended ? 0 : 1 );
			}
		}
	}

	if ( !pWordforms || iTokensGot<(*pWordforms)->m_iMinTokens+1 )
		return m_dStoredTokens[m_iStart].m_sToken;

	int iStartToken = m_iStart + ( bBlended ? 1 : 0 );
	for ( int i = (*pWordforms)->m_pForms.GetLength()-1; i>=0; i-- )
	{
		const CSphMultiform * pCurForm = (*pWordforms)->m_pForms[i];
		int iFormTokCount = pCurForm->m_dTokens.GetLength();

		if ( iTokensGot<iFormTokCount+1 || ( bBlended && iTokensGot!=iFormTokCount+1 ) )
			continue;

		int iForm = 0;
		for ( ; iForm<iFormTokCount; iForm++ )
		{
			const StoredToken_t & tTok = m_dStoredTokens[iStartToken + 1 + iForm];
			const char * szStored = (const char*)tTok.m_sToken;
			const char * szNormal = pCurForm->m_dTokens[iForm].cstr ();

			if ( *szNormal!=*szStored || strcasecmp ( szNormal, szStored ) )
				break;
		}

		// early out - no destination form detected
		if ( iForm!=iFormTokCount )
			continue;

		// tokens after folded form are valid tail that should be processed next time
		if ( m_bBuildMultiform )
		{
			BYTE * pOut = m_sTokenizedMultiform;
			BYTE * pMax = pOut + sizeof(m_sTokenizedMultiform);
			for ( int j=0; j<iFormTokCount+1 && pOut<pMax; j++ )
			{
				const StoredToken_t & tTok = m_dStoredTokens[iStartToken+j];
				const BYTE * sTok = tTok.m_sToken;
				if ( j && pOut<pMax )
					*pOut++ = ' ';
				while ( *sTok && pOut<pMax )
					*pOut++ = *sTok++;
			}
			*pOut = '\0';
			*(pMax-1) = '\0';
		}

		if ( !bBlended )
		{
			// fold regular tokens to form
			StoredToken_t & tStart = m_dStoredTokens[m_iStart];
			StoredToken_t & tEnd = m_dStoredTokens[m_iStart+iFormTokCount];
			m_iStart += iFormTokCount;

			strncpy ( (char *)tEnd.m_sToken, pCurForm->m_sNormalForm.cstr(), sizeof(tEnd.m_sToken) );
			tEnd.m_szTokenStart = tStart.m_szTokenStart;
			tEnd.m_pBufferPtr = tStart.m_pBufferPtr;
			tEnd.m_iTokenLen = pCurForm->m_iNormalTokenLen;

			tEnd.m_bBoundary = false;
			tEnd.m_bSpecial = false;
			tEnd.m_bBlended = false;
			tEnd.m_bBlendedPart = false;
		} else
		{
			// replace blended by form
			StoredToken_t & tDst = m_dStoredTokens[m_iStart];
			strncpy ( (char *)tDst.m_sToken, pCurForm->m_sNormalForm.cstr(), sizeof(tDst.m_sToken) );
			tDst.m_iTokenLen = pCurForm->m_iNormalTokenLen;
		}
	}

	return m_dStoredTokens[m_iStart].m_sToken;
}


ISphTokenizer * CSphMultiformTokenizer::Clone ( ESphTokenizerClone eMode ) const
{
	ISphTokenizer * pClone = m_pTokenizer->Clone ( eMode );
	return CreateMultiformFilter ( pClone, m_pMultiWordforms );
}


void CSphMultiformTokenizer::SetBufferPtr ( const char * sNewPtr )
{
	m_iStart = 0;
	m_dStoredTokens.Resize ( 0 );
	m_pTokenizer->SetBufferPtr ( sNewPtr );
}

void CSphMultiformTokenizer::SetBuffer ( const BYTE * sBuffer, int iLength )
{
	m_pTokenizer->SetBuffer ( sBuffer, iLength );
	SetBufferPtr ( (const char *)sBuffer );
}

uint64_t CSphMultiformTokenizer::GetSettingsFNV () const
{
	uint64_t uHash = CSphTokenFilter::GetSettingsFNV();
	uHash ^= (uint64_t)m_pMultiWordforms;
	return uHash;
}


int CSphMultiformTokenizer::SkipBlended ()
{
	bool bGotBlended = ( m_iStart<m_dStoredTokens.GetLength() &&
		( m_dStoredTokens[m_iStart].m_bBlended || m_dStoredTokens[m_iStart].m_bBlendedPart ) );
	if ( !bGotBlended )
		return 0;

	int iWasStart = m_iStart;
	for ( int iTok=m_iStart+1; iTok<m_dStoredTokens.GetLength() && m_dStoredTokens[iTok].m_bBlendedPart && !m_dStoredTokens[iTok].m_bBlended; iTok++ )
		m_iStart = iTok;

	return m_iStart-iWasStart;
}


/////////////////////////////////////////////////////////////////////////////
// FILTER
/////////////////////////////////////////////////////////////////////////////

CSphFilterSettings::CSphFilterSettings ()
	: m_sAttrName	( "" )
	, m_bExclude	( false )
	, m_bHasEqual		( true )
	, m_iMinValue	( LLONG_MIN )
	, m_iMaxValue	( LLONG_MAX )
	, m_pValues		( NULL )
	, m_nValues		( 0 )
{}


CSphFilterSettings::CSphFilterSettings ( const CSphFilterSettings & rhs )
{
	assert ( 0 );
	(*this) = rhs;
}


void CSphFilterSettings::SetExternalValues ( const SphAttr_t * pValues, int nValues )
{
	m_pValues = pValues;
	m_nValues = nValues;
}


bool CSphFilterSettings::operator == ( const CSphFilterSettings & rhs ) const
{
	// check name, mode, type
	if ( m_sAttrName!=rhs.m_sAttrName || m_bExclude!=rhs.m_bExclude || m_eType!=rhs.m_eType )
		return false;

	switch ( m_eType )
	{
		case SPH_FILTER_RANGE:
			return m_iMinValue==rhs.m_iMinValue && m_iMaxValue==rhs.m_iMaxValue;

		case SPH_FILTER_FLOATRANGE:
			return m_fMinValue==rhs.m_fMinValue && m_fMaxValue==rhs.m_fMaxValue;

		case SPH_FILTER_VALUES:
			if ( m_dValues.GetLength()!=rhs.m_dValues.GetLength() )
				return false;

			ARRAY_FOREACH ( i, m_dValues )
				if ( m_dValues[i]!=rhs.m_dValues[i] )
					return false;

			return true;

		default:
			assert ( 0 && "internal error: unhandled filter type in comparison" );
			return false;
	}
}

/////////////////////////////////////////////////////////////////////////////
// QUERY
/////////////////////////////////////////////////////////////////////////////

CSphQuery::CSphQuery ()
	: m_sIndexes	( "*" )
	, m_sQuery		( "" )
	, m_sRawQuery	( "" )
	, m_iOffset		( 0 )
	, m_iLimit		( 20 )
	, m_pWeights	( NULL )
	, m_iWeights	( 0 )
	, m_eMode		( SPH_MATCH_EXTENDED )
	, m_eRanker		( SPH_RANK_DEFAULT )
	, m_eSort		( SPH_SORT_RELEVANCE )
	, m_iMaxMatches	( 1000 )
	, m_bSortKbuffer	( false )
	, m_bZSlist			( false )
	, m_bSimplify		( false )
	, m_bPlainIDF		( false )
	, m_bGlobalIDF		( false )
	, m_bNormalizedTFIDF ( true )
	, m_bLocalDF		( false )
	, m_eGroupFunc		( SPH_GROUPBY_ATTR )
	, m_sGroupSortBy	( "@groupby desc" )
	, m_sGroupDistinct	( "" )
	, m_iCutoff			( 0 )
	, m_iRetryCount		( 0 )
	, m_iRetryDelay		( 0 )
	, m_iAgentQueryTimeout	( 0 )
	, m_bGeoAnchor		( false )
	, m_fGeoLatitude	( 0.0f )
	, m_fGeoLongitude	( 0.0f )
	, m_uMaxQueryMsec	( 0 )
	, m_iMaxPredictedMsec ( 0 )
	, m_sComment		( "" )
	, m_sSelect			( "" )
	, m_iOuterOffset	( 0 )
	, m_iOuterLimit		( 0 )
	, m_bHasOuter		( false )
	, m_bReverseScan	( false )
	, m_bIgnoreNonexistent ( false )
	, m_bIgnoreNonexistentIndexes ( false )
	, m_bStrict			( false )
	, m_pTableFunc		( NULL )

	, m_iSQLSelectStart	( -1 )
	, m_iSQLSelectEnd	( -1 )
	, m_iGroupbyLimit	( 1 )

	, m_iOldVersion		( 0 )
	, m_iOldGroups		( 0 )
	, m_pOldGroups		( NULL )
	, m_iOldMinTS		( 0 )
	, m_iOldMaxTS		( UINT_MAX )
	, m_iOldMinGID		( 0 )
	, m_iOldMaxGID		( UINT_MAX )

	, m_eCollation		( SPH_COLLATION_DEFAULT )
	, m_bAgent			( false )
{}


CSphQuery::~CSphQuery ()
{
}


//////////////////////////////////////////////////////////////////////////

struct SelectBounds_t
{
	int		m_iStart;
	int		m_iEnd;
};
#define YYSTYPE SelectBounds_t
#include "yysphinxselect.h"


class SelectParser_t
{
public:
	int				GetToken ( YYSTYPE * lvalp );
	void			AddItem ( YYSTYPE * pExpr, ESphAggrFunc eAggrFunc=SPH_AGGR_NONE, YYSTYPE * pStart=NULL, YYSTYPE * pEnd=NULL );
	void			AddItem ( const char * pToken, YYSTYPE * pStart=NULL, YYSTYPE * pEnd=NULL );
	void			AliasLastItem ( YYSTYPE * pAlias );
	void			AddOption ( YYSTYPE * pOpt, YYSTYPE * pVal );

private:
	void			AutoAlias ( CSphQueryItem & tItem, YYSTYPE * pStart, YYSTYPE * pEnd );
	bool			IsTokenEqual ( YYSTYPE * pTok, const char * sRef );

public:
	CSphString		m_sParserError;
	const char *	m_pLastTokenStart;

	const char *	m_pStart;
	const char *	m_pCur;

	CSphQuery *		m_pQuery;
};

int yylex ( YYSTYPE * lvalp, SelectParser_t * pParser )
{
	return pParser->GetToken ( lvalp );
}

void yyerror ( SelectParser_t * pParser, const char * sMessage )
{
	pParser->m_sParserError.SetSprintf ( "%s near '%s'", sMessage, pParser->m_pLastTokenStart );
}

#include "yysphinxselect.c"


int SelectParser_t::GetToken ( YYSTYPE * lvalp )
{
	// skip whitespace, check eof
	while ( isspace ( *m_pCur ) )
		m_pCur++;
	if ( !*m_pCur )
		return 0;

	// begin working that token
	m_pLastTokenStart = m_pCur;
	lvalp->m_iStart = m_pCur-m_pStart;

	// check for constant
	if ( isdigit ( *m_pCur ) )
	{
		char * pEnd = NULL;
		double fDummy; // to avoid gcc unused result warning
		fDummy = strtod ( m_pCur, &pEnd );
		fDummy *= 2; // to avoid gcc unused variable warning

		m_pCur = pEnd;
		lvalp->m_iEnd = m_pCur-m_pStart;
		return SEL_TOKEN;
	}

	// check for token
	if ( sphIsAttr ( m_pCur[0] ) || ( m_pCur[0]=='@' && sphIsAttr ( m_pCur[1] ) && !isdigit ( m_pCur[1] ) ) )
	{
		m_pCur++;
		while ( sphIsAttr ( *m_pCur ) ) m_pCur++;
		lvalp->m_iEnd = m_pCur-m_pStart;

		#define LOC_CHECK(_str,_len,_ret) \
			if ( lvalp->m_iEnd==_len+lvalp->m_iStart && strncasecmp ( m_pStart+lvalp->m_iStart, _str, _len )==0 ) return _ret;

		LOC_CHECK ( "ID", 2, SEL_ID );
		LOC_CHECK ( "AS", 2, SEL_AS );
		LOC_CHECK ( "OR", 2, TOK_OR );
		LOC_CHECK ( "AND", 3, TOK_AND );
		LOC_CHECK ( "NOT", 3, TOK_NOT );
		LOC_CHECK ( "DIV", 3, TOK_DIV );
		LOC_CHECK ( "MOD", 3, TOK_MOD );
		LOC_CHECK ( "AVG", 3, SEL_AVG );
		LOC_CHECK ( "MIN", 3, SEL_MIN );
		LOC_CHECK ( "MAX", 3, SEL_MAX );
		LOC_CHECK ( "SUM", 3, SEL_SUM );
		LOC_CHECK ( "GROUP_CONCAT", 12, SEL_GROUP_CONCAT );
		LOC_CHECK ( "GROUPBY", 7, SEL_GROUPBY );
		LOC_CHECK ( "COUNT", 5, SEL_COUNT );
		LOC_CHECK ( "DISTINCT", 8, SEL_DISTINCT );
		LOC_CHECK ( "WEIGHT", 6, SEL_WEIGHT );
		LOC_CHECK ( "OPTION", 6, SEL_OPTION );

		#undef LOC_CHECK

		return SEL_TOKEN;
	}

	// check for equality checks
	lvalp->m_iEnd = 1+lvalp->m_iStart;
	switch ( *m_pCur )
	{
		case '<':
			m_pCur++;
			if ( *m_pCur=='>' ) { m_pCur++; lvalp->m_iEnd++; return TOK_NE; }
			if ( *m_pCur=='=' ) { m_pCur++; lvalp->m_iEnd++; return TOK_LTE; }
			return '<';

		case '>':
			m_pCur++;
			if ( *m_pCur=='=' ) { m_pCur++; lvalp->m_iEnd++; return TOK_GTE; }
			return '>';

		case '=':
			m_pCur++;
			if ( *m_pCur=='=' ) { m_pCur++; lvalp->m_iEnd++; }
			return TOK_EQ;

		case '\'':
		{
			const char cEnd = *m_pCur;
			for ( const char * s = m_pCur+1; *s; s++ )
			{
				if ( *s==cEnd )
				{
					m_pCur = s+1;
					return TOK_CONST_STRING;
				}
			}
			return -1;
		}
	}

	// check for comment begin/end
	if ( m_pCur[0]=='/' && m_pCur[1]=='*' )
	{
		m_pCur += 2;
		lvalp->m_iEnd += 1;
		return SEL_COMMENT_OPEN;
	}
	if ( m_pCur[0]=='*' && m_pCur[1]=='/' )
	{
		m_pCur += 2;
		lvalp->m_iEnd += 1;
		return SEL_COMMENT_CLOSE;
	}

	// return char as a token
	return *m_pCur++;
}

void SelectParser_t::AutoAlias ( CSphQueryItem & tItem, YYSTYPE * pStart, YYSTYPE * pEnd )
{
	if ( pStart && pEnd )
	{
		tItem.m_sAlias.SetBinary ( m_pStart + pStart->m_iStart, pEnd->m_iEnd - pStart->m_iStart );
		sphColumnToLowercase ( const_cast<char *>( tItem.m_sAlias.cstr() ) ); // as in SqlParser_c
	} else
		tItem.m_sAlias = tItem.m_sExpr;
}

void SelectParser_t::AddItem ( YYSTYPE * pExpr, ESphAggrFunc eAggrFunc, YYSTYPE * pStart, YYSTYPE * pEnd )
{
	CSphQueryItem & tItem = m_pQuery->m_dItems.Add();
	tItem.m_sExpr.SetBinary ( m_pStart + pExpr->m_iStart, pExpr->m_iEnd - pExpr->m_iStart );
	sphColumnToLowercase ( const_cast<char *>( tItem.m_sExpr.cstr() ) );
	tItem.m_eAggrFunc = eAggrFunc;
	AutoAlias ( tItem, pStart, pEnd );
}

void SelectParser_t::AddItem ( const char * pToken, YYSTYPE * pStart, YYSTYPE * pEnd )
{
	CSphQueryItem & tItem = m_pQuery->m_dItems.Add();
	tItem.m_sExpr = pToken;
	tItem.m_eAggrFunc = SPH_AGGR_NONE;
	sphColumnToLowercase ( const_cast<char *>( tItem.m_sExpr.cstr() ) );
	AutoAlias ( tItem, pStart, pEnd );
}

void SelectParser_t::AliasLastItem ( YYSTYPE * pAlias )
{
	if ( pAlias )
	{
		CSphQueryItem & tItem = m_pQuery->m_dItems.Last();
		tItem.m_sAlias.SetBinary ( m_pStart + pAlias->m_iStart, pAlias->m_iEnd - pAlias->m_iStart );
		tItem.m_sAlias.ToLower();
	}
}

bool SelectParser_t::IsTokenEqual ( YYSTYPE * pTok, const char * sRef )
{
	int iLen = strlen(sRef);
	if ( iLen!=( pTok->m_iEnd - pTok->m_iStart ) )
		return false;
	return strncasecmp ( m_pStart + pTok->m_iStart, sRef, iLen )==0;
}

void SelectParser_t::AddOption ( YYSTYPE * pOpt, YYSTYPE * pVal )
{
	if ( IsTokenEqual ( pOpt, "reverse_scan" ) )
	{
		if ( IsTokenEqual ( pVal, "1" ) )
			m_pQuery->m_bReverseScan = true;
	} else if ( IsTokenEqual ( pOpt, "sort_method" ) )
	{
		if ( IsTokenEqual ( pVal, "kbuffer" ) )
			m_pQuery->m_bSortKbuffer = true;
	} else if ( IsTokenEqual ( pOpt, "max_predicted_time" ) )
	{
		char szNumber[256];
		int iLen = pVal->m_iEnd-pVal->m_iStart;
		assert ( iLen < (int)sizeof(szNumber) );
		strncpy ( szNumber, m_pStart+pVal->m_iStart, iLen );
		int64_t iMaxPredicted = strtoull ( szNumber, NULL, 10 );
		m_pQuery->m_iMaxPredictedMsec = int(iMaxPredicted > INT_MAX ? INT_MAX : iMaxPredicted );
	}
}

bool CSphQuery::ParseSelectList ( CSphString & sError )
{
	m_dItems.Reset ();
	if ( m_sSelect.IsEmpty() )
		return true; // empty is ok; will just return everything

	SelectParser_t tParser;
	tParser.m_pStart = m_sSelect.cstr();
	tParser.m_pCur = tParser.m_pStart;
	tParser.m_pQuery = this;

	yyparse ( &tParser );

	sError = tParser.m_sParserError;
	return sError.IsEmpty ();
}

/////////////////////////////////////////////////////////////////////////////
// QUERY STATS
/////////////////////////////////////////////////////////////////////////////

CSphQueryStats::CSphQueryStats()
	: m_pNanoBudget ( NULL )
	, m_iFetchedDocs ( 0 )
	, m_iFetchedHits ( 0 )
	, m_iSkips ( 0 )
{
}

void CSphQueryStats::Add ( const CSphQueryStats & tStats )
{
	m_iFetchedDocs += tStats.m_iFetchedDocs;
	m_iFetchedHits += tStats.m_iFetchedHits;
	m_iSkips += tStats.m_iSkips;
}


/////////////////////////////////////////////////////////////////////////////
// SCHEMAS
/////////////////////////////////////////////////////////////////////////////

static CSphString sphDumpAttr ( const CSphColumnInfo & tAttr )
{
	CSphString sRes;
	sRes.SetSprintf ( "%s %s:%d@%d", sphTypeName ( tAttr.m_eAttrType ), tAttr.m_sName.cstr(),
		tAttr.m_tLocator.m_iBitCount, tAttr.m_tLocator.m_iBitOffset );
	return sRes;
}


/// make string lowercase but keep case of JSON.field
void sphColumnToLowercase ( char * sVal )
{
	if ( !sVal || !*sVal )
		return;

	// make all chars lowercase but only prior to '.' and '[' delimiters
	for ( ; *sVal && *sVal!='.' && *sVal!='['; sVal++ )
		*sVal = (char) tolower ( *sVal );
}


CSphColumnInfo::CSphColumnInfo ( const char * sName, ESphAttr eType )
	: m_sName ( sName )
	, m_eAttrType ( eType )
	, m_eWordpart ( SPH_WORDPART_WHOLE )
	, m_bIndexed ( false )
	, m_iIndex ( -1 )
	, m_eSrc ( SPH_ATTRSRC_NONE )
	, m_pExpr ( NULL )
	, m_eAggrFunc ( SPH_AGGR_NONE )
	, m_eStage ( SPH_EVAL_STATIC )
	, m_bPayload ( false )
	, m_bFilename ( false )
	, m_bWeight ( false )
	, m_uNext ( 0xffff )
{
	sphColumnToLowercase ( const_cast<char *>( m_sName.cstr() ) );
}

//////////////////////////////////////////////////////////////////////////

void ISphSchema::Reset()
{
	m_dPtrAttrs.Reset();
	m_dFactorAttrs.Reset();
}


void ISphSchema::InsertAttr ( CSphVector<CSphColumnInfo> & dAttrs, CSphVector<int> & dUsed, int iPos, const CSphColumnInfo & tCol, bool bDynamic )
{
	assert ( 0<=iPos && iPos<=dAttrs.GetLength() );
	assert ( tCol.m_eAttrType!=SPH_ATTR_NONE && !tCol.m_tLocator.IsID() ); // not via this orifice bro
	if ( tCol.m_eAttrType==SPH_ATTR_NONE || tCol.m_tLocator.IsID() )
		return;

	dAttrs.Insert ( iPos, tCol );
	CSphAttrLocator & tLoc = dAttrs [ iPos ].m_tLocator;

	int iBits = ROWITEM_BITS;
	if ( tLoc.m_iBitCount>0 )
		iBits = tLoc.m_iBitCount;
	if ( tCol.m_eAttrType==SPH_ATTR_BOOL )
		iBits = 1;
	if ( tCol.m_eAttrType==SPH_ATTR_BIGINT || tCol.m_eAttrType==SPH_ATTR_JSON_FIELD )
		iBits = 64;

	if ( tCol.m_eAttrType==SPH_ATTR_STRINGPTR || tCol.m_eAttrType==SPH_ATTR_FACTORS )
	{
		assert ( bDynamic );
		iBits = ROWITEMPTR_BITS;
		CSphNamedInt & t = ( tCol.m_eAttrType==SPH_ATTR_STRINGPTR )
			? m_dPtrAttrs.Add()
			: m_dFactorAttrs.Add();
		t.m_iValue = dUsed.GetLength();
		t.m_sName = tCol.m_sName;
	}

	tLoc.m_iBitCount = iBits;
	tLoc.m_bDynamic = bDynamic;

	if ( iBits>=ROWITEM_BITS )
	{
		tLoc.m_iBitOffset = dUsed.GetLength()*ROWITEM_BITS;
		int iItems = (iBits+ROWITEM_BITS-1) / ROWITEM_BITS;
		for ( int i=0; i<iItems; i++ )
			dUsed.Add ( ROWITEM_BITS );
	} else
	{
		int iItem;
		for ( iItem=0; iItem<dUsed.GetLength(); iItem++ )
			if ( dUsed[iItem]+iBits<=ROWITEM_BITS )
				break;
		if ( iItem==dUsed.GetLength() )
			dUsed.Add ( 0 );
		tLoc.m_iBitOffset = iItem*ROWITEM_BITS + dUsed[iItem];
		dUsed[iItem] += iBits;
	}
}


void ISphSchema::CloneWholeMatch ( CSphMatch * pDst, const CSphMatch & rhs ) const
{
	assert ( pDst );
	FreeStringPtrs ( pDst );
	pDst->Combine ( rhs, GetRowSize() );
	CopyPtrs ( pDst, rhs );
}


void ISphSchema::CopyPtrs ( CSphMatch * pDst, const CSphMatch & rhs, int iUpBound ) const
{
	if ( iUpBound<0 )
	{
		ARRAY_FOREACH ( i, m_dPtrAttrs )
			*(const char**) (pDst->m_pDynamic+m_dPtrAttrs[i].m_iValue) = CSphString (*(const char**)(rhs.m_pDynamic+m_dPtrAttrs[i].m_iValue)).Leak();
	} else
	{
		for ( int i=0; i<m_dPtrAttrs.GetLength(); ++i )
			if ( m_dPtrAttrs[i].m_iValue < iUpBound )
				*(const char**) (pDst->m_pDynamic+m_dPtrAttrs[i].m_iValue) = CSphString (*(const char**)(rhs.m_pDynamic+m_dPtrAttrs[i].m_iValue)).Leak();
			else
				break;
	}

	// not immediately obvious: this is not needed while pushing matches to sorters; factors are held in an outer hash table
	// but it is necessary to copy factors when combining results from several indexes via a sorter because at this moment matches are the owners of factor data
	ARRAY_FOREACH ( i, m_dFactorAttrs )
	{
		int iOffset = m_dFactorAttrs[i].m_iValue;
		BYTE * pData = *(BYTE**)(rhs.m_pDynamic+iOffset);
		if ( pData )
		{
			DWORD uDataSize = *(DWORD*)pData;
			assert ( uDataSize );

			BYTE * pCopy = new BYTE[uDataSize];
			memcpy ( pCopy, pData, uDataSize );
			*(BYTE**)(pDst->m_pDynamic+iOffset) = pCopy;
		}
	}
}


void ISphSchema::FreeStringPtrs ( CSphMatch * pMatch, int iLowBound, int iUpBound ) const
{
	assert ( pMatch );
	if ( !pMatch->m_pDynamic )
		return;

	if ( m_dPtrAttrs.GetLength() )
	{
		CSphString sStr;
		if ( iUpBound<0 )
		{
			ARRAY_FOREACH ( i, m_dPtrAttrs )
			{
				if ( m_dPtrAttrs[i].m_iValue<iLowBound )
					continue;
				sStr.Adopt ( (char**) (pMatch->m_pDynamic+m_dPtrAttrs[i].m_iValue));
			}
		} else
		{
			ARRAY_FOREACH ( i, m_dPtrAttrs )
			{
				if ( m_dPtrAttrs[i].m_iValue<iLowBound )
					continue;
				if ( m_dPtrAttrs[i].m_iValue<iUpBound )
					sStr.Adopt ( (char**) (pMatch->m_pDynamic+m_dPtrAttrs[i].m_iValue));
				else
					break;
			}
		}
	}

	ARRAY_FOREACH ( i, m_dFactorAttrs )
	{
		int iOffset = m_dFactorAttrs[i].m_iValue;
		if ( iOffset<iLowBound )
			continue;
		BYTE * pData = *(BYTE**)(pMatch->m_pDynamic+iOffset);
		if ( pData )
		{
			delete [] pData;
			*(BYTE**)(pMatch->m_pDynamic+iOffset) = NULL;
		}
	}
}

//////////////////////////////////////////////////////////////////////////

CSphSchema::CSphSchema ( const char * sName )
	: m_sName ( sName )
	, m_iStaticSize ( 0 )
{
	for ( int i=0; i<BUCKET_COUNT; i++ )
		m_dBuckets[i] = 0xffff;
}


bool CSphSchema::CompareTo ( const CSphSchema & rhs, CSphString & sError, bool bFullComparison ) const
{
	// check attr count
	if ( GetAttrsCount()!=rhs.GetAttrsCount() )
	{
		sError.SetSprintf ( "attribute count mismatch (me=%s, in=%s, myattrs=%d, inattrs=%d)",
			m_sName.cstr(), rhs.m_sName.cstr(),
			GetAttrsCount(), rhs.GetAttrsCount() );
		return false;
	}

	// check attrs
	ARRAY_FOREACH ( i, m_dAttrs )
	{
		const CSphColumnInfo & tAttr1 = rhs.m_dAttrs[i];
		const CSphColumnInfo & tAttr2 = m_dAttrs[i];

		bool bMismatch;
		if ( bFullComparison )
			bMismatch = !(tAttr1==tAttr2);
		else
		{
			ESphAttr eAttr1 = tAttr1.m_eAttrType;
			ESphAttr eAttr2 = tAttr2.m_eAttrType;
			if ( eAttr1==SPH_ATTR_WORDCOUNT )
				eAttr1 = SPH_ATTR_INTEGER;

			if ( eAttr2==SPH_ATTR_WORDCOUNT )
				eAttr2 = SPH_ATTR_INTEGER;

			bMismatch = tAttr1.m_sName!=tAttr2.m_sName || eAttr1!=eAttr2 || tAttr1.m_eWordpart!=tAttr2.m_eWordpart ||
				tAttr1.m_bIndexed!=tAttr2.m_bIndexed ||	tAttr1.m_tLocator.m_iBitCount!=tAttr2.m_tLocator.m_iBitCount ||
				tAttr1.m_tLocator.m_iBitOffset!=tAttr2.m_tLocator.m_iBitOffset;
		}

		if ( bMismatch )
		{
			sError.SetSprintf ( "attribute mismatch (me=%s, in=%s, idx=%d, myattr=%s, inattr=%s)",
				m_sName.cstr(), rhs.m_sName.cstr(), i, sphDumpAttr ( m_dAttrs[i] ).cstr(), sphDumpAttr ( rhs.m_dAttrs[i] ).cstr() );
			return false;
		}
	}

	// check field count
	if ( rhs.m_dFields.GetLength()!=m_dFields.GetLength() )
	{
		sError.SetSprintf ( "fulltext fields count mismatch (me=%s, in=%s, myfields=%d, infields=%d)",
			m_sName.cstr(), rhs.m_sName.cstr(),
			m_dFields.GetLength(), rhs.m_dFields.GetLength() );
		return false;
	}

	// check fulltext field names
	ARRAY_FOREACH ( i, rhs.m_dFields )
		if ( rhs.m_dFields[i].m_sName!=m_dFields[i].m_sName )
	{
		sError.SetSprintf ( "fulltext field mismatch (me=%s, myfield=%s, idx=%d, in=%s, infield=%s)",
			m_sName.cstr(), rhs.m_sName.cstr(),
			i, m_dFields[i].m_sName.cstr(), rhs.m_dFields[i].m_sName.cstr() );
		return false;
	}

	return true;
}


int CSphSchema::GetFieldIndex ( const char * sName ) const
{
	if ( !sName )
		return -1;
	ARRAY_FOREACH ( i, m_dFields )
		if ( strcasecmp ( m_dFields[i].m_sName.cstr(), sName )==0 )
			return i;
	return -1;
}


int CSphSchema::GetAttrIndex ( const char * sName ) const
{
	if ( !sName )
		return -1;

	if ( m_dAttrs.GetLength()>=HASH_THRESH )
	{
		DWORD uCrc = sphCRC32 ( (const BYTE*)sName );
		DWORD uPos = m_dBuckets [ uCrc%BUCKET_COUNT ];
		while ( uPos!=0xffff && m_dAttrs [ uPos ].m_sName!=sName )
			uPos = m_dAttrs [ uPos ].m_uNext;

		return (short)uPos; // 0xffff == -1 is our "end of list" marker
	}

	ARRAY_FOREACH ( i, m_dAttrs )
		if ( m_dAttrs[i].m_sName==sName )
			return i;

	return -1;
}


const CSphColumnInfo * CSphSchema::GetAttr ( const char * sName ) const
{
	int iIndex = GetAttrIndex ( sName );
	if ( iIndex>=0 )
		return &m_dAttrs[iIndex];
	return NULL;
}


void CSphSchema::Reset ()
{
	ISphSchema::Reset();
	m_dFields.Reset();
	m_dAttrs.Reset();
	for ( int i=0; i<BUCKET_COUNT; i++ )
		m_dBuckets[i] = 0xffff;
	m_dStaticUsed.Reset();
	m_dDynamicUsed.Reset();
	m_iStaticSize = 0;
}


void CSphSchema::InsertAttr ( int iPos, const CSphColumnInfo & tCol, bool bDynamic )
{
	// it's redundant in case of AddAttr
	if ( iPos!=m_dAttrs.GetLength() )
		UpdateHash ( iPos-1, 1 );

	ISphSchema::InsertAttr ( m_dAttrs, bDynamic ? m_dDynamicUsed : m_dStaticUsed, iPos, tCol, bDynamic );

	// update static size
	m_iStaticSize = m_dStaticUsed.GetLength();

	// do hash add
	if ( m_dAttrs.GetLength()==HASH_THRESH )
		RebuildHash();
	else if ( m_dAttrs.GetLength()>HASH_THRESH )
	{
		WORD & uPos = GetBucketPos ( m_dAttrs [ iPos ].m_sName.cstr() );
		m_dAttrs [ iPos ].m_uNext = uPos;
		uPos = (WORD)iPos;
	}
}


void CSphSchema::AddAttr ( const CSphColumnInfo & tCol, bool bDynamic )
{
	InsertAttr ( m_dAttrs.GetLength(), tCol, bDynamic );
}


WORD & CSphSchema::GetBucketPos ( const char * sName )
{
	DWORD uCrc = sphCRC32 ( (const BYTE*)sName );
	return m_dBuckets [ uCrc % BUCKET_COUNT ];
}


void CSphSchema::RebuildHash ()
{
	if ( m_dAttrs.GetLength()<HASH_THRESH )
		return;

	for ( int i=0; i<BUCKET_COUNT; i++ )
		m_dBuckets[i] = 0xffff;

	ARRAY_FOREACH ( i, m_dAttrs )
	{
		WORD & uPos = GetBucketPos ( m_dAttrs[i].m_sName.cstr() );
		m_dAttrs[i].m_uNext = uPos;
		uPos = WORD(i);
	}
}


void CSphSchema::UpdateHash ( int iStartIndex, int iAddVal )
{
	if ( m_dAttrs.GetLength()<HASH_THRESH )
		return;

	ARRAY_FOREACH ( i, m_dAttrs )
	{
		WORD & uPos = m_dAttrs[i].m_uNext;
		if ( uPos!=0xffff && uPos>iStartIndex )
			uPos = (WORD)( uPos + iAddVal );
	}
	for ( int i=0; i<BUCKET_COUNT; i++ )
	{
		WORD & uPos = m_dBuckets[i];
		if ( uPos!=0xffff && uPos>iStartIndex )
			uPos = (WORD)( uPos + iAddVal );
	}
}


void CSphSchema::AssignTo ( CSphRsetSchema & lhs ) const
{
	lhs = *this;
}

//////////////////////////////////////////////////////////////////////////

CSphRsetSchema::CSphRsetSchema()
	: m_pIndexSchema ( NULL )
{}


void CSphRsetSchema::Reset ()
{
	ISphSchema::Reset();
	m_pIndexSchema = NULL;
	m_dExtraAttrs.Reset();
	m_dDynamicUsed.Reset();
	m_dFields.Reset();
}


void CSphRsetSchema::AddDynamicAttr ( const CSphColumnInfo & tCol )
{
	ISphSchema::InsertAttr ( m_dExtraAttrs, m_dDynamicUsed, m_dExtraAttrs.GetLength(), tCol, true );
}


int CSphRsetSchema::GetRowSize() const
{
	// we copy over dynamic map in case index schema has dynamic attributes
	// (that happens in case of inline attributes, or RAM segments in RT indexes)
	// so there is no need to add GetDynamicSize() here
	return m_pIndexSchema
		? m_dDynamicUsed.GetLength() + m_pIndexSchema->GetStaticSize()
		: m_dDynamicUsed.GetLength();
}


int CSphRsetSchema::GetStaticSize() const
{
	// result set schemas additions are always dynamic
	return m_pIndexSchema ? m_pIndexSchema->GetStaticSize() : 0;
}


int CSphRsetSchema::GetDynamicSize() const
{
	// we copy over dynamic map in case index schema has dynamic attributes
	return m_dDynamicUsed.GetLength();
}


int CSphRsetSchema::GetAttrsCount() const
{
	return m_pIndexSchema
		? m_dExtraAttrs.GetLength() + m_pIndexSchema->GetAttrsCount() - m_dRemoved.GetLength()
		: m_dExtraAttrs.GetLength();
}


int CSphRsetSchema::GetAttrIndex ( const char * sName ) const
{
	ARRAY_FOREACH ( i, m_dExtraAttrs )
		if ( m_dExtraAttrs[i].m_sName==sName )
			return i + ( m_pIndexSchema ? m_pIndexSchema->GetAttrsCount() - m_dRemoved.GetLength() : 0 );

	if ( !m_pIndexSchema )
		return -1;

	int iRes = m_pIndexSchema->GetAttrIndex(sName);
	if ( iRes>=0 )
	{
		if ( m_dRemoved.Contains ( iRes ) )
			return -1;
		int iSub = 0;
		ARRAY_FOREACH_COND ( i, m_dRemoved, iRes>=m_dRemoved[i] )
			iSub++;
		return iRes - iSub;
	}
	return -1;
}


const CSphColumnInfo & CSphRsetSchema::GetAttr ( int iIndex ) const
{
	if ( !m_pIndexSchema )
		return m_dExtraAttrs[iIndex];

	if ( iIndex < m_pIndexSchema->GetAttrsCount() - m_dRemoved.GetLength() )
	{
		ARRAY_FOREACH_COND ( i, m_dRemoved, iIndex>=m_dRemoved[i] )
			iIndex++;
		return m_pIndexSchema->GetAttr(iIndex);
	}

	return m_dExtraAttrs [ iIndex - m_pIndexSchema->GetAttrsCount() + m_dRemoved.GetLength() ];
}


const CSphColumnInfo * CSphRsetSchema::GetAttr ( const char * sName ) const
{
	ARRAY_FOREACH ( i, m_dExtraAttrs )
		if ( m_dExtraAttrs[i].m_sName==sName )
			return &m_dExtraAttrs[i];
	if ( m_pIndexSchema )
		return m_pIndexSchema->GetAttr ( sName );
	return NULL;
}


CSphRsetSchema & CSphRsetSchema::operator = ( const ISphSchema & rhs )
{
	rhs.AssignTo ( *this );
	return *this;
}


CSphRsetSchema & CSphRsetSchema::operator = ( const CSphSchema & rhs )
{
	Reset();
	m_dFields = rhs.m_dFields; // OPTIMIZE? sad but copied
	m_pIndexSchema = &rhs;

	// copy over dynamic rowitems map
	// so that the new attributes we might add would not overlap
	m_dDynamicUsed = rhs.m_dDynamicUsed;
	return *this;
}


void CSphRsetSchema::RemoveStaticAttr ( int iAttr )
{
	assert ( m_pIndexSchema );
	assert ( iAttr>=0 );
	assert ( iAttr<( m_pIndexSchema->GetAttrsCount() - m_dRemoved.GetLength() ) );

	// map from rset indexes (adjusted for removal) to index schema indexes (the original ones)
	ARRAY_FOREACH_COND ( i, m_dRemoved, iAttr>=m_dRemoved[i] )
		iAttr++;
	m_dRemoved.Add ( iAttr );
	m_dRemoved.Uniq();
}


void CSphRsetSchema::SwapAttrs ( CSphVector<CSphColumnInfo> & dAttrs )
{
#ifndef NDEBUG
	// ensure that every incoming column has a matching original column
	// only check locators and attribute types, because at this stage,
	// names that are used in dAttrs are already overwritten by the aliases
	// (example: SELECT col1 a, col2 b, count(*) c FROM test)
	//
	// FIXME? maybe also lockdown the schema from further swaps, adds etc from here?
	ARRAY_FOREACH ( i, dAttrs )
	{
		if ( dAttrs[i].m_tLocator.IsID() )
			continue;
		bool bFound1 = false;
		if ( m_pIndexSchema )
		{
			const CSphVector<CSphColumnInfo> & dSrc = m_pIndexSchema->m_dAttrs;
			bFound1 = ARRAY_ANY ( bFound1, dSrc, dSrc[_any].m_tLocator==dAttrs[i].m_tLocator && dSrc[_any].m_eAttrType==dAttrs[i].m_eAttrType )
		}
		bool bFound2 = ARRAY_ANY ( bFound2, m_dExtraAttrs,
			m_dExtraAttrs[_any].m_tLocator==dAttrs[i].m_tLocator && m_dExtraAttrs[_any].m_eAttrType==dAttrs[i].m_eAttrType )
			assert ( bFound1 || bFound2 );
	}
#endif
	m_dExtraAttrs.SwapData ( dAttrs );
	m_pIndexSchema = NULL;
}


void CSphRsetSchema::CloneMatch ( CSphMatch * pDst, const CSphMatch & rhs, int iUpBound ) const
{
	assert ( pDst );
	FreeStringPtrs ( pDst, 0, iUpBound );
	pDst->Combine ( rhs, GetDynamicSize(), iUpBound );
	CopyPtrs ( pDst, rhs, iUpBound );
}


void CSphRsetSchema::CombineMatch ( CSphMatch * pDst, const CSphMatch & rhs1, const CSphMatch & rhs2, int iUpBound ) const
{
	assert ( pDst );
	FreeStringPtrs ( pDst, (pDst==&rhs1)?iUpBound:0 );
	pDst->Combine ( rhs1, GetDynamicSize(), iUpBound, &rhs2 );

	// combine ptrs
	ARRAY_FOREACH ( i, m_dPtrAttrs )
	{
		const CSphMatch & rhs = ( m_dPtrAttrs[i].m_iValue < iUpBound ) ? rhs1 : rhs2;
		if ( pDst!=&rhs )
			*(const char**) (pDst->m_pDynamic+m_dPtrAttrs[i].m_iValue) = CSphString (*(const char**)(rhs.m_pDynamic+m_dPtrAttrs[i].m_iValue)).Leak();
	}

	// not immediately obvious: this is not needed while pushing matches to sorters; factors are held in an outer hash table
	// but it is necessary to copy factors when combining results from several indexes via a sorter because at this moment matches are the owners of factor data
	if ( pDst!=&rhs1 )
		ARRAY_FOREACH ( i, m_dFactorAttrs )
	{
		int iOffset = m_dFactorAttrs[i].m_iValue;
		BYTE * pData = *(BYTE**)(rhs1.m_pDynamic+iOffset);
		if ( pData )
		{
			DWORD uDataSize = *(DWORD*)pData;
			assert ( uDataSize );

			BYTE * pCopy = new BYTE[uDataSize];
			memcpy ( pCopy, pData, uDataSize );
			*(BYTE**)(pDst->m_pDynamic+iOffset) = pCopy;
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
// BIT-ENCODED FILE OUTPUT
///////////////////////////////////////////////////////////////////////////////

CSphWriter::CSphWriter ()
	: m_sName ( "" )
	, m_iPos ( -1 )
	, m_iWritten ( 0 )

	, m_iFD ( -1 )
	, m_iPoolUsed ( 0 )
	, m_pBuffer ( NULL )
	, m_pPool ( NULL )
	, m_bOwnFile ( false )
	, m_pSharedOffset ( NULL )
	, m_iBufferSize	( 262144 )

	, m_bError ( false )
	, m_pError ( NULL )
{
	m_pThrottle = &g_tThrottle;
}


void CSphWriter::SetBufferSize ( int iBufferSize )
{
	if ( iBufferSize!=m_iBufferSize )
	{
		m_iBufferSize = Max ( iBufferSize, 262144 );
		SafeDeleteArray ( m_pBuffer );
	}
}


bool CSphWriter::OpenFile ( const CSphString & sName, CSphString & sErrorBuffer )
{
	assert ( !sName.IsEmpty() );
	assert ( m_iFD<0 && "already open" );

	m_bOwnFile = true;
	m_sName = sName;
	m_pError = &sErrorBuffer;

	if ( !m_pBuffer )
		m_pBuffer = new BYTE [ m_iBufferSize ];

	m_iFD = ::open ( m_sName.cstr(), SPH_O_NEW, 0644 );
	m_pPool = m_pBuffer;
	m_iPoolUsed = 0;
	m_iPos = 0;
	m_iWritten = 0;
	m_bError = ( m_iFD<0 );

	if ( m_bError )
		m_pError->SetSprintf ( "failed to create %s: %s" , sName.cstr(), strerror(errno) );

	return !m_bError;
}


void CSphWriter::SetFile ( CSphAutofile & tAuto, SphOffset_t * pSharedOffset, CSphString & sError )
{
	assert ( m_iFD<0 && "already open" );
	m_bOwnFile = false;

	if ( !m_pBuffer )
		m_pBuffer = new BYTE [ m_iBufferSize ];

	m_iFD = tAuto.GetFD();
	m_sName = tAuto.GetFilename();
	m_pPool = m_pBuffer;
	m_iPoolUsed = 0;
	m_iPos = 0;
	m_iWritten = 0;
	m_pSharedOffset = pSharedOffset;
	m_pError = &sError;
	assert ( m_pError );
}


CSphWriter::~CSphWriter ()
{
	CloseFile ();
	SafeDeleteArray ( m_pBuffer );
}


void CSphWriter::CloseFile ( bool bTruncate )
{
	if ( m_iFD>=0 )
	{
		Flush ();
		if ( bTruncate )
			sphTruncate ( m_iFD );
		if ( m_bOwnFile )
			::close ( m_iFD );
		m_iFD = -1;
	}
}

void CSphWriter::UnlinkFile()
{
	if ( m_bOwnFile )
	{
		if ( m_iFD>=0 )
			::close ( m_iFD );

		m_iFD = -1;
		::unlink ( m_sName.cstr() );
		m_sName = "";
	}
	SafeDeleteArray ( m_pBuffer );
}


void CSphWriter::PutByte ( int data )
{
	assert ( m_pPool );
	if ( m_iPoolUsed==m_iBufferSize )
		Flush ();
	*m_pPool++ = BYTE ( data & 0xff );
	m_iPoolUsed++;
	m_iPos++;
}


void CSphWriter::PutBytes ( const void * pData, int64_t iSize )
{
	assert ( m_pPool );
	const BYTE * pBuf = (const BYTE *) pData;
	while ( iSize>0 )
	{
		int iPut = ( iSize<m_iBufferSize ? int(iSize) : m_iBufferSize ); // comparison int64 to int32
		if ( m_iPoolUsed+iPut>m_iBufferSize )
			Flush ();
		assert ( m_iPoolUsed+iPut<=m_iBufferSize );

		memcpy ( m_pPool, pBuf, iPut );
		m_pPool += iPut;
		m_iPoolUsed += iPut;
		m_iPos += iPut;

		pBuf += iPut;
		iSize -= iPut;
	}
}


void CSphWriter::ZipInt ( DWORD uValue )
{
	int iBytes = 1;

	DWORD u = ( uValue>>7 );
	while ( u )
	{
		u >>= 7;
		iBytes++;
	}

	while ( iBytes-- )
		PutByte (
			( 0x7f & ( uValue >> (7*iBytes) ) )
			| ( iBytes ? 0x80 : 0 ) );
}


void CSphWriter::ZipOffset ( SphOffset_t uValue )
{
	int iBytes = 1;

	uint64_t u = ((uint64_t)uValue)>>7;
	while ( u )
	{
		u >>= 7;
		iBytes++;
	}

	while ( iBytes-- )
		PutByte (
			( 0x7f & (DWORD)( uValue >> (7*iBytes) ) )
			| ( iBytes ? 0x80 : 0 ) );
}


void CSphWriter::ZipOffsets ( CSphVector<SphOffset_t> * pData )
{
	assert ( pData );

	SphOffset_t * pValue = &((*pData)[0]);
	int n = pData->GetLength ();

	while ( n-->0 )
	{
		SphOffset_t uValue = *pValue++;

		int iBytes = 1;

		uint64_t u = ((uint64_t)uValue)>>7;
		while ( u )
		{
			u >>= 7;
			iBytes++;
		}

		while ( iBytes-- )
			PutByte (
				( 0x7f & (DWORD)( uValue >> (7*iBytes) ) )
				| ( iBytes ? 0x80 : 0 ) );
	}
}


void CSphWriter::Flush ()
{
	if ( m_pSharedOffset && *m_pSharedOffset!=m_iWritten )
		sphSeek ( m_iFD, m_iWritten, SEEK_SET );

	if ( !sphWriteThrottled ( m_iFD, m_pBuffer, m_iPoolUsed, m_sName.cstr(), *m_pError, m_pThrottle ) )
		m_bError = true;

	m_iWritten += m_iPoolUsed;
	m_iPoolUsed = 0;
	m_pPool = m_pBuffer;

	if ( m_pSharedOffset )
		*m_pSharedOffset = m_iWritten;
}


void CSphWriter::PutString ( const char * szString )
{
	int iLen = szString ? strlen ( szString ) : 0;
	PutDword ( iLen );
	if ( iLen )
		PutBytes ( szString, iLen );
}


void CSphWriter::PutString ( const CSphString & sString )
{
	int iLen = sString.Length();
	PutDword ( iLen );
	if ( iLen )
		PutBytes ( sString.cstr(), iLen );
}


void CSphWriter::Tag ( const char * sTag )
{
	assert ( sTag && *sTag ); // empty tags are nonsense
	assert ( strlen(sTag)<64 ); // huge tags are nonsense
	PutBytes ( sTag, strlen(sTag) );
}


void CSphWriter::SeekTo ( SphOffset_t iPos )
{
	assert ( iPos>=0 );

	if ( iPos>=m_iWritten && iPos<=( m_iWritten + m_iPoolUsed ) )
	{
		// seeking inside the buffer
		m_iPoolUsed = (int)( iPos - m_iWritten );
		m_pPool = m_pBuffer + m_iPoolUsed;
	} else
	{
		assert ( iPos<m_iWritten ); // seeking forward in a writer, we don't support it
		sphSeek ( m_iFD, iPos, SEEK_SET );

		// seeking outside the buffer; so the buffer must be discarded
		// also, current write position must be adjusted
		m_pPool = m_pBuffer;
		m_iPoolUsed = 0;
		m_iWritten = iPos;
	}
	m_iPos = iPos;
}

///////////////////////////////////////////////////////////////////////////////
// BIT-ENCODED FILE INPUT
///////////////////////////////////////////////////////////////////////////////

CSphReader::CSphReader ( BYTE * pBuf, int iSize )
	: m_pProfile ( NULL )
	, m_eProfileState ( SPH_QSTATE_IO )
	, m_iFD ( -1 )
	, m_iPos ( 0 )
	, m_iBuffPos ( 0 )
	, m_iBuffUsed ( 0 )
	, m_pBuff ( pBuf )
	, m_iSizeHint ( 0 )
	, m_iBufSize ( iSize )
	, m_bBufOwned ( false )
	, m_iReadUnhinted ( DEFAULT_READ_UNHINTED )
	, m_bError ( false )
{
	assert ( pBuf==NULL || iSize>0 );
	m_pThrottle = &g_tThrottle;
}


CSphReader::~CSphReader ()
{
	if ( m_bBufOwned )
		SafeDeleteArray ( m_pBuff );
}


void CSphReader::SetBuffers ( int iReadBuffer, int iReadUnhinted )
{
	if ( !m_pBuff )
		m_iBufSize = iReadBuffer;
	m_iReadUnhinted = iReadUnhinted;
}


void CSphReader::SetFile ( int iFD, const char * sFilename )
{
	m_iFD = iFD;
	m_iPos = 0;
	m_iBuffPos = 0;
	m_iBuffUsed = 0;
	m_sFilename = sFilename;
}


void CSphReader::SetFile ( const CSphAutofile & tFile )
{
	SetFile ( tFile.GetFD(), tFile.GetFilename() );
}


void CSphReader::Reset ()
{
	SetFile ( -1, "" );
}


/// sizehint > 0 means we expect to read approx that much bytes
/// sizehint == 0 means no hint, use default (happens later in UpdateCache())
/// sizehint == -1 means reposition and adjust current hint
void CSphReader::SeekTo ( SphOffset_t iPos, int iSizeHint )
{
	assert ( iPos>=0 );
	assert ( iSizeHint>=-1 );

#ifndef NDEBUG
#if PARANOID
	struct_stat tStat;
	fstat ( m_iFD, &tStat );
	if ( iPos > tStat.st_size )
		sphDie ( "INTERNAL ERROR: seeking past the end of file" );
#endif
#endif

	if ( iPos>=m_iPos && iPos<m_iPos+m_iBuffUsed )
	{
		m_iBuffPos = (int)( iPos-m_iPos ); // reposition to proper byte
		m_iSizeHint = iSizeHint - ( m_iBuffUsed - m_iBuffPos ); // we already have some bytes cached, so let's adjust size hint
		assert ( m_iBuffPos<m_iBuffUsed );
	} else
	{
		m_iPos = iPos;
		m_iBuffPos = 0; // for GetPos() to work properly, aaaargh
		m_iBuffUsed = 0;

		if ( iSizeHint==-1 )
		{
			// the adjustment bureau
			// we need to seek but still keep the current hint
			// happens on a skiplist jump, for instance
			int64_t iHintLeft = m_iPos + m_iSizeHint - iPos;
			if ( iHintLeft>0 && iHintLeft<INT_MAX )
				iSizeHint = (int)iHintLeft;
			else
				iSizeHint = 0;
		}

		// get that hint
		assert ( iSizeHint>=0 );
		m_iSizeHint = iSizeHint;
	}
}


void CSphReader::SkipBytes ( int iCount )
{
	// 0 means "no hint", so this clamp works alright
	SeekTo ( m_iPos+m_iBuffPos+iCount, Max ( m_iSizeHint-m_iBuffPos-iCount, 0 ) );
}


#if USE_WINDOWS

// atomic seek+read for Windows
int sphPread ( int iFD, void * pBuf, int iBytes, SphOffset_t iOffset )
{
	if ( iBytes==0 )
		return 0;

	CSphIOStats * pIOStats = GetIOStats();
	int64_t tmStart = 0;
	if ( pIOStats )
		tmStart = sphMicroTimer();

	HANDLE hFile;
	hFile = (HANDLE) _get_osfhandle ( iFD );
	if ( hFile==INVALID_HANDLE_VALUE )
		return -1;

	STATIC_SIZE_ASSERT ( SphOffset_t, 8 );
	OVERLAPPED tOverlapped = { 0 };
	tOverlapped.Offset = (DWORD)( iOffset & I64C(0xffffffff) );
	tOverlapped.OffsetHigh = (DWORD)( iOffset>>32 );

	DWORD uRes;
	if ( !ReadFile ( hFile, pBuf, iBytes, &uRes, &tOverlapped ) )
	{
		DWORD uErr = GetLastError();
		if ( uErr==ERROR_HANDLE_EOF )
			return 0;

		errno = uErr; // FIXME! should remap from Win to POSIX
		return -1;
	}

	if ( pIOStats )
	{
		pIOStats->m_iReadTime += sphMicroTimer() - tmStart;
		pIOStats->m_iReadOps++;
		pIOStats->m_iReadBytes += iBytes;
	}

	return uRes;
}

#else
#if HAVE_PREAD

// atomic seek+read for non-Windows systems with pread() call
int sphPread ( int iFD, void * pBuf, int iBytes, SphOffset_t iOffset )
{
	CSphIOStats * pIOStats = GetIOStats();
	if ( !pIOStats )
		return ::pread ( iFD, pBuf, iBytes, iOffset );

	int64_t tmStart = sphMicroTimer();
	int iRes = (int) ::pread ( iFD, pBuf, iBytes, iOffset );
	if ( pIOStats )
	{
		pIOStats->m_iReadTime += sphMicroTimer() - tmStart;
		pIOStats->m_iReadOps++;
		pIOStats->m_iReadBytes += iBytes;
	}
	return iRes;
}

#else

// generic fallback; prone to races between seek and read
int sphPread ( int iFD, void * pBuf, int iBytes, SphOffset_t iOffset )
{
	if ( sphSeek ( iFD, iOffset, SEEK_SET )==-1 )
		return -1;

	return sphReadThrottled ( iFD, pBuf, iBytes, &g_tThrottle );
}

#endif // HAVE_PREAD
#endif // USE_WINDOWS


void CSphReader::UpdateCache ()
{
	ESphQueryState eOld = SPH_QSTATE_TOTAL;
	if ( m_pProfile )
		eOld = m_pProfile->Switch ( m_eProfileState );

	assert ( m_iFD>=0 );

	// alloc buf on first actual read
	if ( !m_pBuff )
	{
		if ( m_iBufSize<=0 )
			m_iBufSize = DEFAULT_READ_BUFFER;

		m_bBufOwned = true;
		m_pBuff = new BYTE [ m_iBufSize ];
	}

	// stream position could be changed externally
	// so let's just hope that the OS optimizes redundant seeks
	SphOffset_t iNewPos = m_iPos + Min ( m_iBuffPos, m_iBuffUsed );

	if ( m_iSizeHint<=0 )
		m_iSizeHint = ( m_iReadUnhinted>0 ) ? m_iReadUnhinted : DEFAULT_READ_UNHINTED;
	int iReadLen = Min ( m_iSizeHint, m_iBufSize );

	m_iBuffPos = 0;
	m_iBuffUsed = sphPread ( m_iFD, m_pBuff, iReadLen, iNewPos ); // FIXME! what about throttling?

	if ( m_iBuffUsed<0 )
	{
		m_iBuffUsed = m_iBuffPos = 0;
		m_bError = true;
		m_sError.SetSprintf ( "pread error in %s: pos="INT64_FMT", len=%d, code=%d, msg=%s",
			m_sFilename.cstr(), (int64_t)iNewPos, iReadLen, errno, strerror(errno) );
		if ( m_pProfile )
			m_pProfile->Switch ( eOld );
		return;
	}

	// all fine, adjust offset and hint
	m_iSizeHint -= m_iBuffUsed;
	m_iPos = iNewPos;
	if ( m_pProfile )
		m_pProfile->Switch ( eOld );
}


int CSphReader::GetByte ()
{
	if ( m_iBuffPos>=m_iBuffUsed )
	{
		UpdateCache ();
		if ( m_iBuffPos>=m_iBuffUsed )
			return 0; // unexpected io failure
	}

	assert ( m_iBuffPos<m_iBuffUsed );
	return m_pBuff [ m_iBuffPos++ ];
}


void CSphReader::GetBytes ( void * pData, int iSize )
{
	BYTE * pOut = (BYTE*) pData;

	while ( iSize>m_iBufSize )
	{
		int iLen = m_iBuffUsed - m_iBuffPos;
		assert ( iLen<=m_iBufSize );

		memcpy ( pOut, m_pBuff+m_iBuffPos, iLen );
		m_iBuffPos += iLen;
		pOut += iLen;
		iSize -= iLen;
		m_iSizeHint = iSize; // FIXME!

		if ( iSize>0 )
		{
			UpdateCache ();
			if ( !m_iBuffUsed )
			{
				memset ( pData, 0, iSize );
				return; // unexpected io failure
			}
		}
	}

	if ( m_iBuffPos+iSize>m_iBuffUsed )
	{
		// move old buffer tail to buffer head to avoid losing the data
		const int iLen = m_iBuffUsed - m_iBuffPos;
		if ( iLen>0 )
		{
			memcpy ( pOut, m_pBuff+m_iBuffPos, iLen );
			m_iBuffPos += iLen;
			pOut += iLen;
			iSize -= iLen;
		}

		m_iSizeHint = iSize - m_iBuffUsed + m_iBuffPos; // FIXME!
		UpdateCache ();
		if ( m_iBuffPos+iSize>m_iBuffUsed )
		{
			memset ( pData, 0, iSize ); // unexpected io failure
			return;
		}
	}

	assert ( (m_iBuffPos+iSize)<=m_iBuffUsed );
	memcpy ( pOut, m_pBuff+m_iBuffPos, iSize );
	m_iBuffPos += iSize;
}


int CSphReader::GetBytesZerocopy ( const BYTE ** ppData, int iMax )
{
	if ( m_iBuffPos>=m_iBuffUsed )
	{
		UpdateCache ();
		if ( m_iBuffPos>=m_iBuffUsed )
			return 0; // unexpected io failure
	}

	int iChunk = Min ( m_iBuffUsed-m_iBuffPos, iMax );
	*ppData = m_pBuff + m_iBuffPos;
	m_iBuffPos += iChunk;
	return iChunk;
}


int CSphReader::GetLine ( char * sBuffer, int iMaxLen )
{
	int iOutPos = 0;
	iMaxLen--; // reserve space for trailing '\0'

	// grab as many chars as we can
	while ( iOutPos<iMaxLen )
	{
		// read next chunk if necessary
		if ( m_iBuffPos>=m_iBuffUsed )
		{
			UpdateCache ();
			if ( m_iBuffPos>=m_iBuffUsed )
			{
				if ( iOutPos==0 ) return -1; // current line is empty; indicate eof
				break; // return current line; will return eof next time
			}
		}

		// break on CR or LF
		if ( m_pBuff[m_iBuffPos]=='\r' || m_pBuff[m_iBuffPos]=='\n' )
			break;

		// one more valid char
		sBuffer[iOutPos++] = m_pBuff[m_iBuffPos++];
	}

	// skip everything until the newline or eof
	for ( ;; )
	{
		// read next chunk if necessary
		if ( m_iBuffPos>=m_iBuffUsed )
			UpdateCache ();

		// eof?
		if ( m_iBuffPos>=m_iBuffUsed )
			break;

		// newline?
		if ( m_pBuff[m_iBuffPos++]=='\n' )
			break;
	}

	// finalize
	sBuffer[iOutPos] = '\0';
	return iOutPos;
}

/////////////////////////////////////////////////////////////////////////////

#if PARANOID

#define SPH_VARINT_DECODE(_type,_getexpr) \
	register DWORD b = 0; \
	register _type v = 0; \
	int it = 0; \
	do { b = _getexpr; v = ( v<<7 ) + ( b&0x7f ); it++; } while ( b&0x80 ); \
	assert ( (it-1)*7<=sizeof(_type)*8 ); \
	return v;

#else

#define SPH_VARINT_DECODE(_type,_getexpr) \
	register DWORD b = _getexpr; \
	register _type res = 0; \
	while ( b & 0x80 ) \
	{ \
		res = ( res<<7 ) + ( b & 0x7f ); \
		b = _getexpr; \
	} \
	res = ( res<<7 ) + b; \
	return res;

#endif // PARANOID

DWORD sphUnzipInt ( const BYTE * & pBuf )			{ SPH_VARINT_DECODE ( DWORD, *pBuf++ ); }
SphOffset_t sphUnzipOffset ( const BYTE * & pBuf )	{ SPH_VARINT_DECODE ( SphOffset_t, *pBuf++ ); }

DWORD CSphReader::UnzipInt ()			{ SPH_VARINT_DECODE ( DWORD, GetByte() ); }
SphOffset_t CSphReader::UnzipOffset ()	{ SPH_VARINT_DECODE ( uint64_t, GetByte() ); }


#if USE_64BIT
#define sphUnzipWordid sphUnzipOffset
#else
#define sphUnzipWordid sphUnzipInt
#endif

/////////////////////////////////////////////////////////////////////////////

const CSphReader & CSphReader::operator = ( const CSphReader & rhs )
{
	SetFile ( rhs.m_iFD, rhs.m_sFilename.cstr() );
	SeekTo ( rhs.m_iPos + rhs.m_iBuffPos, rhs.m_iSizeHint );
	return *this;
}


DWORD CSphReader::GetDword ()
{
	DWORD uRes = 0;
	GetBytes ( &uRes, sizeof(DWORD) );
	return uRes;
}


SphOffset_t CSphReader::GetOffset ()
{
	SphOffset_t uRes = 0;
	GetBytes ( &uRes, sizeof(SphOffset_t) );
	return uRes;
}


CSphString CSphReader::GetString ()
{
	CSphString sRes;

	DWORD iLen = GetDword ();
	if ( iLen )
	{
		char * sBuf = new char [ iLen ];
		GetBytes ( sBuf, iLen );
		sRes.SetBinary ( sBuf, iLen );
		SafeDeleteArray ( sBuf );
	}

	return sRes;
}

bool CSphReader::Tag ( const char * sTag )
{
	if ( m_bError )
		return false;

	assert ( sTag && *sTag ); // empty tags are nonsense
	assert ( strlen(sTag)<64 ); // huge tags are nonsense

	int iLen = strlen(sTag);
	char sBuf[64];
	GetBytes ( sBuf, iLen );
	if ( !memcmp ( sBuf, sTag, iLen ) )
		return true;
	m_bError = true;
	m_sError.SetSprintf ( "expected tag %s was not found", sTag );
	return false;
}

//////////////////////////////////////////////////////////////////////////

CSphAutoreader::~CSphAutoreader ()
{
	Close ();
}


bool CSphAutoreader::Open ( const CSphString & sFilename, CSphString & sError )
{
	assert ( m_iFD<0 );
	assert ( !sFilename.IsEmpty() );

	m_iFD = ::open ( sFilename.cstr(), SPH_O_READ, 0644 );
	m_iPos = 0;
	m_iBuffPos = 0;
	m_iBuffUsed = 0;
	m_sFilename = sFilename;

	if ( m_iFD<0 )
		sError.SetSprintf ( "failed to open %s: %s", sFilename.cstr(), strerror(errno) );
	return ( m_iFD>=0 );
}


void CSphAutoreader::Close ()
{
	if ( m_iFD>=0 )
		::close ( m_iFD	);
	m_iFD = -1;
}


SphOffset_t CSphAutoreader::GetFilesize ()
{
	assert ( m_iFD>=0 );

	struct_stat st;
	if ( m_iFD<0 || fstat ( m_iFD, &st )<0 )
		return -1;

	return st.st_size;
}

/////////////////////////////////////////////////////////////////////////////
// QUERY RESULT
/////////////////////////////////////////////////////////////////////////////

CSphQueryResult::CSphQueryResult ()
{
	m_iQueryTime = 0;
	m_iRealQueryTime = 0;
	m_iCpuTime = 0;
	m_iMultiplier = 1;
	m_iTotalMatches = 0;
	m_pMva = NULL;
	m_pStrings = NULL;
	m_iOffset = 0;
	m_iCount = 0;
	m_iSuccesses = 0;
	m_pProfile = NULL;
}


CSphQueryResult::~CSphQueryResult ()
{
	ARRAY_FOREACH ( i, m_dStorage2Free )
	{
		SafeDeleteArray ( m_dStorage2Free[i] );
	}
	ARRAY_FOREACH ( i, m_dMatches )
		m_tSchema.FreeStringPtrs ( &m_dMatches[i] );
}

void CSphQueryResult::LeakStorages ( CSphQueryResult & tDst )
{
	ARRAY_FOREACH ( i, m_dStorage2Free )
		tDst.m_dStorage2Free.Add ( m_dStorage2Free[i] );

	m_dStorage2Free.Reset();
}


/////////////////////////////////////////////////////////////////////////////
// CHUNK READER
/////////////////////////////////////////////////////////////////////////////

CSphBin::CSphBin ( ESphHitless eMode, bool bWordDict )
	: m_eMode ( eMode )
	, m_dBuffer ( NULL )
	, m_pCurrent ( NULL )
	, m_iLeft ( 0 )
	, m_iDone ( 0 )
	, m_eState ( BIN_POS )
	, m_bWordDict ( bWordDict )
	, m_bError ( false )
	, m_iFile ( -1 )
	, m_pFilePos ( NULL )
	, m_iFilePos ( 0 )
	, m_iFileLeft ( 0 )
{
	m_tHit.m_sKeyword = bWordDict ? m_sKeyword : NULL;
	m_sKeyword[0] = '\0';
	m_pThrottle = &g_tThrottle;

#ifndef NDEBUG
	m_iLastWordID = 0;
	m_sLastKeyword[0] = '\0';
#endif
}


int CSphBin::CalcBinSize ( int iMemoryLimit, int iBlocks, const char * sPhase, bool bWarn )
{
	if ( iBlocks<=0 )
		return CSphBin::MIN_SIZE;

	int iBinSize = ( ( iMemoryLimit/iBlocks + 2048 ) >> 12 ) << 12; // round to 4k

	if ( iBinSize<CSphBin::MIN_SIZE )
	{
		iBinSize = CSphBin::MIN_SIZE;
		sphWarn ( "%s: mem_limit=%d kb extremely low, increasing to %d kb",
			sPhase, iMemoryLimit/1024, iBinSize*iBlocks/1024 );
	}

	if ( iBinSize<CSphBin::WARN_SIZE && bWarn )
	{
		sphWarn ( "%s: merge_block_size=%d kb too low, increasing mem_limit may improve performance",
			sPhase, iBinSize/1024 );
	}

	return iBinSize;
}


void CSphBin::Init ( int iFD, SphOffset_t * pSharedOffset, const int iBinSize )
{
	assert ( !m_dBuffer );
	assert ( iBinSize>=MIN_SIZE );
	assert ( pSharedOffset );

	m_iFile = iFD;
	m_pFilePos = pSharedOffset;

	m_iSize = iBinSize;
	m_dBuffer = new BYTE [ iBinSize ];
	m_pCurrent = m_dBuffer;

	m_tHit.m_iDocID = 0;
	m_tHit.m_iWordID = 0;
	m_tHit.m_iWordPos = EMPTY_HIT;
	m_tHit.m_dFieldMask.UnsetAll();

	m_bError = false;
}


CSphBin::~CSphBin ()
{
	SafeDeleteArray ( m_dBuffer );
}


int CSphBin::ReadByte ()
{
	BYTE r;

	if ( !m_iLeft )
	{
		if ( *m_pFilePos!=m_iFilePos )
		{
			sphSeek ( m_iFile, m_iFilePos, SEEK_SET );
			*m_pFilePos = m_iFilePos;
		}

		int n = m_iFileLeft > m_iSize
			? m_iSize
			: (int)m_iFileLeft;
		if ( n==0 )
		{
			m_iDone = 1;
			m_iLeft = 1;
		} else
		{
			assert ( m_dBuffer );

			if ( sphReadThrottled ( m_iFile, m_dBuffer, n, m_pThrottle )!=(size_t)n )
			{
				m_bError = true;
				return -2;
			}
			m_iLeft = n;

			m_iFilePos += n;
			m_iFileLeft -= n;
			m_pCurrent = m_dBuffer;
			*m_pFilePos += n;
		}
	}
	if ( m_iDone )
	{
		m_bError = true; // unexpected (!) eof
		return -1;
	}

	m_iLeft--;
	r = *(m_pCurrent);
	m_pCurrent++;
	return r;
}


ESphBinRead CSphBin::ReadBytes ( void * pDest, int iBytes )
{
	assert ( iBytes>0 );
	assert ( iBytes<=m_iSize );

	if ( m_iDone )
		return BIN_READ_EOF;

	if ( m_iLeft<iBytes )
	{
		if ( *m_pFilePos!=m_iFilePos )
		{
			sphSeek ( m_iFile, m_iFilePos, SEEK_SET );
			*m_pFilePos = m_iFilePos;
		}

		int n = Min ( m_iFileLeft, m_iSize - m_iLeft );
		if ( n==0 )
		{
			m_iDone = 1;
			m_bError = true; // unexpected (!) eof
			return BIN_READ_EOF;
		}

		assert ( m_dBuffer );
		memmove ( m_dBuffer, m_pCurrent, m_iLeft );

		if ( sphReadThrottled ( m_iFile, m_dBuffer + m_iLeft, n, m_pThrottle )!=(size_t)n )
		{
			m_bError = true;
			return BIN_READ_ERROR;
		}

		m_iLeft += n;
		m_iFilePos += n;
		m_iFileLeft -= n;
		m_pCurrent = m_dBuffer;
		*m_pFilePos += n;
	}

	assert ( m_iLeft>=iBytes );
	m_iLeft -= iBytes;

	memcpy ( pDest, m_pCurrent, iBytes );
	m_pCurrent += iBytes;

	return BIN_READ_OK;
}


SphWordID_t CSphBin::ReadVLB ()
{
	SphWordID_t uValue = 0;
	int iByte, iOffset = 0;
	do
	{
		if ( ( iByte = ReadByte() )<0 )
			return 0;
		uValue += ( ( SphWordID_t ( iByte & 0x7f ) ) << iOffset );
		iOffset += 7;
	}
	while ( iByte & 0x80 );
	return uValue;
}

DWORD CSphBin::UnzipInt ()
{
	register int b = 0;
	register DWORD v = 0;
	do
	{
		b = ReadByte();
		if ( b<0 )
			b = 0;
		v = ( v<<7 ) + ( b & 0x7f );
	} while ( b & 0x80 );
	return v;
}

SphOffset_t CSphBin::UnzipOffset ()
{
	register int b = 0;
	register SphOffset_t v = 0;
	do
	{
		b = ReadByte();
		if ( b<0 )
			b = 0;
		v = ( v<<7 ) + ( b & 0x7f );
	} while ( b & 0x80 );
	return v;
}

int CSphBin::ReadHit ( CSphAggregateHit * pOut, int iRowitems, CSphRowitem * pRowitems )
{
	// expected EOB
	if ( m_iDone )
	{
		pOut->m_iWordID = 0;
		return 1;
	}

	CSphAggregateHit & tHit = m_tHit; // shortcut
	for ( ;; )
	{
		// SPH_MAX_WORD_LEN is now 42 only to keep ReadVLB() below
		// technically, we can just use different functions on different paths, if ever needed
		STATIC_ASSERT ( SPH_MAX_WORD_LEN*3<=127, KEYWORD_TOO_LONG );
		SphWordID_t uDelta = ReadVLB();

		if ( uDelta )
		{
			switch ( m_eState )
			{
				case BIN_WORD:
					if ( m_bWordDict )
					{
#ifdef NDEBUG
						// FIXME?! move this under PARANOID or something?
						// or just introduce an assert() checked release build?
						if ( uDelta>=sizeof(m_sKeyword) )
							sphDie ( "INTERNAL ERROR: corrupted keyword length (len="UINT64_FMT", deltapos="UINT64_FMT")",
								(uint64_t)uDelta, (uint64_t)(m_iFilePos-m_iLeft) );
#else
						assert ( uDelta>0 && uDelta<sizeof(m_sKeyword)-1 );
#endif

						ReadBytes ( m_sKeyword, (int)uDelta );
						m_sKeyword[uDelta] = '\0';
						tHit.m_iWordID = sphCRC32 ( m_sKeyword ); // must be in sync with dict!

#ifndef NDEBUG
						assert ( ( m_iLastWordID<tHit.m_iWordID )
							|| ( m_iLastWordID==tHit.m_iWordID && strcmp ( (char*)m_sLastKeyword, (char*)m_sKeyword )<0 ) );
						strncpy ( (char*)m_sLastKeyword, (char*)m_sKeyword, sizeof(m_sLastKeyword) );
#endif

					} else
					{
						tHit.m_iWordID += uDelta;
					}
					tHit.m_iDocID = 0;
					tHit.m_iWordPos = EMPTY_HIT;
					tHit.m_dFieldMask.UnsetAll();
					m_eState = BIN_DOC;
					break;

				case BIN_DOC:
					// doc id
					m_eState = BIN_POS;
					tHit.m_iDocID += uDelta;
					tHit.m_iWordPos = EMPTY_HIT;
					for ( int i=0; i<iRowitems; i++, pRowitems++ )
						*pRowitems = (DWORD)ReadVLB(); // FIXME? check range?
					break;

				case BIN_POS:
					if ( m_eMode==SPH_HITLESS_ALL )
					{
						tHit.m_dFieldMask.Assign32 ( (DWORD)ReadVLB() );
						m_eState = BIN_DOC;

					} else if ( m_eMode==SPH_HITLESS_SOME )
					{
						if ( uDelta & 1 )
						{
							tHit.m_dFieldMask.Assign32 ( (DWORD)ReadVLB() );
							m_eState = BIN_DOC;
						}
						uDelta >>= 1;
					}
					tHit.m_iWordPos += (DWORD)uDelta;
					*pOut = tHit;
					return 1;

				default:
					sphDie ( "INTERNAL ERROR: unknown bin state (state=%d)", m_eState );
			}
		} else
		{
			switch ( m_eState )
			{
				case BIN_POS:	m_eState = BIN_DOC; break;
				case BIN_DOC:	m_eState = BIN_WORD; break;
				case BIN_WORD:	m_iDone = 1; pOut->m_iWordID = 0; return 1;
				default:		sphDie ( "INTERNAL ERROR: unknown bin state (state=%d)", m_eState );
			}
		}
	}
}


bool CSphBin::IsEOF () const
{
	return m_iDone!=0 || m_iFileLeft<=0;
}


bool CSphBin::IsDone () const
{
	return m_iDone!=0 || ( m_iFileLeft<=0 && m_iLeft<=0 );
}


ESphBinRead CSphBin::Precache ()
{
	if ( m_iFileLeft > m_iSize-m_iLeft )
	{
		m_bError = true;
		return BIN_PRECACHE_ERROR;
	}

	if ( !m_iFileLeft )
		return BIN_PRECACHE_OK;

	if ( *m_pFilePos!=m_iFilePos )
	{
		sphSeek ( m_iFile, m_iFilePos, SEEK_SET );
		*m_pFilePos = m_iFilePos;
	}

	assert ( m_dBuffer );
	memmove ( m_dBuffer, m_pCurrent, m_iLeft );

	if ( sphReadThrottled ( m_iFile, m_dBuffer+m_iLeft, m_iFileLeft, m_pThrottle )!=(size_t)m_iFileLeft )
	{
		m_bError = true;
		return BIN_READ_ERROR;
	}

	m_iLeft += m_iFileLeft;
	m_iFilePos += m_iFileLeft;
	m_iFileLeft -= m_iFileLeft;
	m_pCurrent = m_dBuffer;
	*m_pFilePos += m_iFileLeft;

	return BIN_PRECACHE_OK;
}


//////////////////////////////////////////////////////////////////////////
// INDEX SETTINGS
//////////////////////////////////////////////////////////////////////////

CSphIndexSettings::CSphIndexSettings ()
	: m_eDocinfo			( SPH_DOCINFO_NONE )
	, m_eHitFormat			( SPH_HIT_FORMAT_PLAIN )
	, m_bHtmlStrip			( false )
	, m_eHitless			( SPH_HITLESS_NONE )
	, m_bVerbose			( false )
	, m_iEmbeddedLimit		( 0 )
	, m_eBigramIndex		( SPH_BIGRAM_NONE )
	, m_uAotFilterMask		( 0 )
	, m_eChineseRLP			( SPH_RLP_NONE )
{
}

//////////////////////////////////////////////////////////////////////////
// GLOBAL MVA STORAGE ARENA
//////////////////////////////////////////////////////////////////////////

class tTester : public ISphNoncopyable
{
public:
	virtual void Reset() = 0;
	virtual void TestData ( int iData ) = 0;
	virtual ~tTester() {}
};

/// shared-memory arena allocator
/// manages small tagged dword strings, upto 4096 bytes in size
class CSphArena
{
public:
							CSphArena ();
							~CSphArena ();

	DWORD *					ReInit ( int uMaxBytes );
	const char *			GetError () const { return m_sError.cstr(); }

	int						TaggedAlloc ( int iTag, int iBytes );
	void					TaggedFreeIndex ( int iTag, int iIndex );
	void					TaggedFreeTag ( int iTag );

	void					ExamineTag ( tTester* pTest, int iTag );

protected:
	static const int		MIN_BITS	= 4;
	static const int		MAX_BITS	= 12;
	static const int		NUM_SIZES	= MAX_BITS-MIN_BITS+2;	///< one for 0 (empty pages), and one for each size from min to max

	static const int		PAGE_SIZE	= 1<<MAX_BITS;
	static const int		PAGE_ALLOCS	= 1<<( MAX_BITS-MIN_BITS);
	static const int		PAGE_BITMAP	= ( PAGE_ALLOCS+8*sizeof(DWORD)-1 )/( 8*sizeof(DWORD) );

	static const int		MAX_TAGS		= 1024;
	static const int		MAX_LOGENTRIES	= 29;

	///< page descriptor
	struct PageDesc_t
	{
		int					m_iSizeBits;			///< alloc size
		int					m_iPrev;				///< prev free page of this size
		int					m_iNext;				///< next free page of this size
		int					m_iUsed;				///< usage count
		DWORD				m_uBitmap[PAGE_BITMAP];	///< usage bitmap
	};

	///< tag descriptor
	struct TagDesc_t
	{
		int					m_iTag;					///< tag value
		int					m_iAllocs;				///< active allocs
		int					m_iLogHead;				///< pointer to head allocs log entry
	};

	///< allocs log entry
	struct AllocsLogEntry_t
	{
		int					m_iUsed;
		int					m_iNext;
		int					m_dEntries[MAX_LOGENTRIES];
	};
	STATIC_SIZE_ASSERT ( AllocsLogEntry_t, 124 );

protected:
	DWORD *					Init ( int uMaxBytes );
	int						RawAlloc ( int iBytes );
	void					RawFree ( int iIndex );
	void					RemoveTag ( TagDesc_t * pTag );

protected:
	CSphProcessSharedMutex	m_tProcMutex;
	CSphMutex				m_tThdMutex;

	int						m_iPages;			///< max pages count
	CSphSharedBuffer<DWORD>	m_pArena;			///< arena that stores everything (all other pointers point here)

	PageDesc_t *			m_pPages;			///< page descriptors
	int *					m_pFreelistHeads;	///< free-list heads
	int *					m_pTagCount;
	TagDesc_t *				m_pTags;

	DWORD *					m_pBasePtr;			///< base data storage pointer
	CSphString				m_sError;

#if ARENADEBUG
protected:
	int *					m_pTotalAllocs;
	int *					m_pTotalBytes;

public:
	void					CheckFreelists ();
#else
	inline void				CheckFreelists () {}
#endif // ARENADEBUG
};

class tDocCollector : public tTester
{
	CSphVector<SphDocID_t> * m_dCollection;
public:
	explicit tDocCollector ( CSphVector<SphDocID_t> & dCollection )
		: m_dCollection ( &dCollection )
	{}
	virtual void Reset()
	{
		m_dCollection->Reset();
	}
	virtual void TestData ( int iData )
	{
		if ( !g_pMvaArena )
			return;

		m_dCollection->Add ( *(SphDocID_t*)(g_pMvaArena + iData) );
	}
};

//////////////////////////////////////////////////////////////////////////
CSphArena::CSphArena ()
	: m_iPages ( 0 )
{
	m_tThdMutex.Init();
}


CSphArena::~CSphArena ()
{
	// notify callers that arena no longer exists
	g_pMvaArena = NULL;
	m_tThdMutex.Done();
}

DWORD * CSphArena::ReInit ( int uMaxBytes )
{
	if ( m_iPages!=0 )
	{
		m_pArena.Reset();
		m_iPages = 0;
	}
	return Init ( uMaxBytes );
}

DWORD * CSphArena::Init ( int uMaxBytes )
{
	m_iPages = ( uMaxBytes+PAGE_SIZE-1 ) / PAGE_SIZE;

	int iData = m_iPages*PAGE_SIZE; // data size, bytes
	int iMyTaglist = sizeof(int) + MAX_TAGS*sizeof(TagDesc_t); // int length, TagDesc_t[] tags; NOLINT
	int iMy = m_iPages*sizeof(PageDesc_t) + NUM_SIZES*sizeof(int) + iMyTaglist; // my internal structures size, bytes; NOLINT
#if ARENADEBUG
	iMy += 2*sizeof(int); // debugging counters; NOLINT
#endif

	assert ( iData%sizeof(DWORD)==0 );
	assert ( iMy%sizeof(DWORD)==0 );

	CSphString sError, sWarning;
	if ( m_tProcMutex.GetError() || !m_pArena.Alloc ( (iData+iMy)/sizeof(DWORD), sError, sWarning ) )
	{
		m_iPages = 0;
		if ( m_tProcMutex.GetError() )
			m_sError = m_tProcMutex.GetError();
		else
			m_sError.SetSprintf ( "alloc, error='%s', warning='%s'", sError.cstr(), sWarning.cstr() );
		return NULL;
	}

	// setup internal pointers
	DWORD * pCur = m_pArena.GetWritePtr();

	m_pPages = (PageDesc_t*) pCur;
	pCur += sizeof(PageDesc_t)*m_iPages/sizeof(DWORD);

	m_pFreelistHeads = (int*) pCur;
	pCur += NUM_SIZES; // one for each size, and one extra for zero

	m_pTagCount = (int*) pCur++;
	m_pTags = (TagDesc_t*) pCur;
	pCur += sizeof(TagDesc_t)*MAX_TAGS/sizeof(DWORD);

#if ARENADEBUG
	m_pTotalAllocs = (int*) pCur++;
	m_pTotalBytes = (int*) pCur++;
	*m_pTotalAllocs = 0;
	*m_pTotalBytes = 0;
#endif

	m_pBasePtr = m_pArena.GetWritePtr() + iMy/sizeof(DWORD);
	assert ( m_pBasePtr==pCur );

	// setup initial state
	for ( int i=0; i<m_iPages; i++ )
	{
		m_pPages[i].m_iSizeBits = 0; // fully empty
		m_pPages[i].m_iPrev = ( i>0 ) ? i-1 : -1;
		m_pPages[i].m_iNext = ( i<m_iPages-1 ) ? i+1 : -1;
	}

	m_pFreelistHeads[0] = 0;
	for ( int i=1; i<NUM_SIZES; i++ )
		m_pFreelistHeads[i] = -1;

	*m_pTagCount = 0;

	return m_pBasePtr;
}


int CSphArena::RawAlloc ( int iBytes )
{
	CheckFreelists ();

	if ( iBytes<=0 || iBytes>( ( 1 << MAX_BITS ) - (int)sizeof(int) ) )
		return -1;

	int iSizeBits = sphLog2 ( iBytes+2*sizeof(int)-1 ); // always reserve sizeof(int) for the tag and AllocsLogEntry_t backtrack; NOLINT
	iSizeBits = Max ( iSizeBits, MIN_BITS );
	assert ( iSizeBits>=MIN_BITS && iSizeBits<=MAX_BITS );

	int iSizeSlot = iSizeBits-MIN_BITS+1;
	assert ( iSizeSlot>=1 && iSizeSlot<NUM_SIZES );

	// get semi-free page for this size
	PageDesc_t * pPage = NULL;
	if ( m_pFreelistHeads[iSizeSlot]>=0 )
	{
		// got something in the free-list
		pPage = m_pPages + m_pFreelistHeads[iSizeSlot];

	} else
	{
		// nothing in free-list, alloc next empty one
		if ( m_pFreelistHeads[0]<0 )
			return -1; // out of memory

		// update the page
		pPage = m_pPages + m_pFreelistHeads[0];
		assert ( pPage->m_iPrev==-1 );

		m_pFreelistHeads[iSizeSlot] = m_pFreelistHeads[0];
		m_pFreelistHeads[0] = pPage->m_iNext;
		if ( pPage->m_iNext>=0 )
			m_pPages[pPage->m_iNext].m_iPrev = -1;

		pPage->m_iSizeBits = iSizeBits;
		pPage->m_iUsed = 0;
		pPage->m_iNext = -1;

		CheckFreelists ();

		// setup bitmap
		int iUsedBits = ( 1<<(MAX_BITS-iSizeBits) ); // max-used-bits = page-size/alloc-size = ( 1<<page-bitsize )/( 1<<alloc-bitsize )
		assert ( iUsedBits>0 && iUsedBits<=(PAGE_BITMAP<<5) );

		for ( int i=0; i<PAGE_BITMAP; i++ )
			pPage->m_uBitmap[i] = ( ( i<<5 )>=iUsedBits ) ? 0xffffffffUL : 0;

		if ( iUsedBits<32 )
			pPage->m_uBitmap[0] = ( 0xffffffffUL<<iUsedBits );
	}

	// get free alloc slot and use it
	assert ( pPage );
	assert ( pPage->m_iSizeBits==iSizeBits );

	for ( int i=0; i<PAGE_BITMAP; i++ ) // FIXME! optimize, can scan less
	{
		if ( pPage->m_uBitmap[i]==0xffffffffUL )
			continue;

		int iFree = FindBit ( pPage->m_uBitmap[i] );
		pPage->m_uBitmap[i] |= ( 1<<iFree );

		pPage->m_iUsed++;
		if ( pPage->m_iUsed==( PAGE_SIZE >> pPage->m_iSizeBits ) )
		{
			// this page is full now, unchain from the free-list
			assert ( m_pFreelistHeads[iSizeSlot]==pPage-m_pPages );
			m_pFreelistHeads[iSizeSlot] = pPage->m_iNext;
			if ( pPage->m_iNext>=0 )
			{
				assert ( m_pPages[pPage->m_iNext].m_iPrev==pPage-m_pPages );
				m_pPages[pPage->m_iNext].m_iPrev = -1;
			}
			pPage->m_iNext = -1;
		}

#if ARENADEBUG
		(*m_pTotalAllocs)++;
		(*m_pTotalBytes) += ( 1<<iSizeBits );
#endif

		CheckFreelists ();

		int iOffset = ( pPage-m_pPages )*PAGE_SIZE + ( i*32+iFree )*( 1<<iSizeBits ); // raw internal byte offset (FIXME! optimize with shifts?)
		int iIndex = 2 + ( iOffset/sizeof(DWORD) ); // dword index with tag and backtrack fixup

		m_pBasePtr[iIndex-1] = DWORD(-1); // untagged by default
		m_pBasePtr[iIndex-2] = DWORD(-1); // backtrack nothere
		return iIndex;
	}

	assert ( 0 && "internal error, no free slots in free page" );
	return -1;
}


void CSphArena::RawFree ( int iIndex )
{
	CheckFreelists ();

	int iOffset = (iIndex-2)*sizeof(DWORD); // remove tag fixup, and go to raw internal byte offset
	int iPage = iOffset / PAGE_SIZE;

	if ( iPage<0 || iPage>m_iPages )
	{
		assert ( 0 && "internal error, freed index out of arena" );
		return;
	}

	PageDesc_t * pPage = m_pPages + iPage;
	int iBit = ( iOffset % PAGE_SIZE ) >> pPage->m_iSizeBits;
	assert ( ( iOffset % PAGE_SIZE )==( iBit << pPage->m_iSizeBits ) && "internal error, freed offset is unaligned" );

	if (!( pPage->m_uBitmap[iBit>>5] & ( 1UL<<(iBit & 31) ) ))
	{
		assert ( 0 && "internal error, freed index already freed" );
		return;
	}

	pPage->m_uBitmap[iBit>>5] &= ~( 1UL << ( iBit & 31 ) );
	pPage->m_iUsed--;

#if ARENADEBUG
	(*m_pTotalAllocs)--;
	(*m_pTotalBytes) -= ( 1<<pPage->m_iSizeBits );
#endif

	CheckFreelists ();

	int iSizeSlot = pPage->m_iSizeBits-MIN_BITS+1;

	if ( pPage->m_iUsed==( PAGE_SIZE >> pPage->m_iSizeBits )-1 )
	{
		// this page was full, but it's semi-free now
		// chain to free-list
		assert ( pPage->m_iPrev==-1 ); // full pages must not be in any list
		assert ( pPage->m_iNext==-1 );

		pPage->m_iNext = m_pFreelistHeads[iSizeSlot];
		if ( pPage->m_iNext>=0 )
		{
			assert ( m_pPages[pPage->m_iNext].m_iPrev==-1 );
			assert ( m_pPages[pPage->m_iNext].m_iSizeBits==pPage->m_iSizeBits );
			m_pPages[pPage->m_iNext].m_iPrev = iPage;
		}
		m_pFreelistHeads[iSizeSlot] = iPage;
	}

	if ( pPage->m_iUsed==0 )
	{
		// this page is empty now
		// unchain from free-list
		if ( pPage->m_iPrev>=0 )
		{
			// non-head page
			assert ( m_pPages[pPage->m_iPrev].m_iNext==iPage );
			m_pPages[pPage->m_iPrev].m_iNext = pPage->m_iNext;

			if ( pPage->m_iNext>=0 )
			{
				assert ( m_pPages[pPage->m_iNext].m_iPrev==iPage );
				m_pPages[pPage->m_iNext].m_iPrev = pPage->m_iPrev;
			}

		} else
		{
			// head page
			assert ( m_pFreelistHeads[iSizeSlot]==iPage );
			assert ( pPage->m_iPrev==-1 );

			if ( pPage->m_iNext>=0 )
			{
				assert ( m_pPages[pPage->m_iNext].m_iPrev==iPage );
				m_pPages[pPage->m_iNext].m_iPrev = -1;
			}
			m_pFreelistHeads[iSizeSlot] = pPage->m_iNext;
		}

		pPage->m_iSizeBits = 0;
		pPage->m_iPrev = -1;
		pPage->m_iNext = m_pFreelistHeads[0];
		if ( pPage->m_iNext>=0 )
		{
			assert ( m_pPages[pPage->m_iNext].m_iPrev==-1 );
			assert ( m_pPages[pPage->m_iNext].m_iSizeBits==0 );
			m_pPages[pPage->m_iNext].m_iPrev = iPage;
		}
		m_pFreelistHeads[0] = iPage;
	}

	CheckFreelists ();
}


int CSphArena::TaggedAlloc ( int iTag, int iBytes )
{
	if ( !m_iPages )
		return -1; // uninitialized

	assert ( iTag>=0 );
	CSphScopedLock<CSphMutex> tThdLock ( m_tThdMutex );
	CSphScopedLock<CSphProcessSharedMutex> tProcLock ( m_tProcMutex );

	// find that tag first
	TagDesc_t * pTag = sphBinarySearch ( m_pTags, m_pTags+(*m_pTagCount)-1, bind ( &TagDesc_t::m_iTag ), iTag );
	if ( !pTag )
	{
		if ( *m_pTagCount==MAX_TAGS )
			return -1; // out of tags

		int iLogHead = RawAlloc ( sizeof(AllocsLogEntry_t) );
		if ( iLogHead<0 )
			return -1; // out of memory

		assert ( iLogHead>=2 );
		AllocsLogEntry_t * pLog = (AllocsLogEntry_t*) ( m_pBasePtr + iLogHead );
		pLog->m_iUsed = 0;
		pLog->m_iNext = -1;

		// add new tag
		pTag = m_pTags + (*m_pTagCount)++;
		pTag->m_iTag = iTag;
		pTag->m_iAllocs = 0;
		pTag->m_iLogHead = iLogHead;

		// re-sort
		// OPTIMIZE! full-blown sort is overkill here
		sphSort ( m_pTags, *m_pTagCount, sphMemberLess ( &TagDesc_t::m_iTag ) );

		// we must be able to find it now
		pTag = sphBinarySearch ( m_pTags, m_pTags+(*m_pTagCount)-1, bind ( &TagDesc_t::m_iTag ), iTag );
		assert ( pTag && "internal error, fresh tag not found in TaggedAlloc()" );

		if ( !pTag )
			return -1; // internal error
	}

	// grow the log if needed
	int iLogEntry = pTag->m_iLogHead;
	AllocsLogEntry_t * pLog = (AllocsLogEntry_t*) ( m_pBasePtr + pTag->m_iLogHead );
	if ( pLog->m_iUsed==MAX_LOGENTRIES )
	{
		int iNewEntry = RawAlloc ( sizeof(AllocsLogEntry_t) );
		if ( iNewEntry<0 )
			return -1; // out of memory

		assert ( iNewEntry>=2 );
		iLogEntry = iNewEntry;
		AllocsLogEntry_t * pNew = (AllocsLogEntry_t*) ( m_pBasePtr + iNewEntry );
		pNew->m_iUsed = 0;
		pNew->m_iNext = pTag->m_iLogHead;
		pTag->m_iLogHead = iNewEntry;
		pLog = pNew;
	}

	// do the alloc itself
	int iIndex = RawAlloc ( iBytes );
	if ( iIndex<0 )
		return -1; // out of memory

	assert ( iIndex>=2 );
	// tag it
	m_pBasePtr[iIndex-1] = iTag;
	// set data->AllocsLogEntry_t backtrack
	m_pBasePtr[iIndex-2] = iLogEntry;

	// log it
	assert ( pLog->m_iUsed<MAX_LOGENTRIES );
	pLog->m_dEntries [ pLog->m_iUsed++ ] = iIndex;
	pTag->m_iAllocs++;

	// and we're done
	return iIndex;
}


void CSphArena::TaggedFreeIndex ( int iTag, int iIndex )
{
	if ( !m_iPages )
		return; // uninitialized

	assert ( iTag>=0 );
	CSphScopedLock<CSphProcessSharedMutex> tProcLock ( m_tProcMutex );
	CSphScopedLock<CSphMutex> tThdLock ( m_tThdMutex );

	// find that tag
	TagDesc_t * pTag = sphBinarySearch ( m_pTags, m_pTags+(*m_pTagCount)-1, bind ( &TagDesc_t::m_iTag ), iTag );
	assert ( pTag && "internal error, unknown tag in TaggedFreeIndex()" );
	assert ( m_pBasePtr[iIndex-1]==DWORD(iTag) && "internal error, tag mismatch in TaggedFreeIndex()" );

	// defence against internal errors
	if ( !pTag )
		return;

	// untag it
	m_pBasePtr[iIndex-1] = DWORD(-1);

	// free it
	RawFree ( iIndex );

	// update AllocsLogEntry_t
	int iLogEntry = m_pBasePtr[iIndex-2];
	assert ( iLogEntry>=2 );
	m_pBasePtr[iIndex-2] = DWORD(-1);
	AllocsLogEntry_t * pLogEntry = (AllocsLogEntry_t*) ( m_pBasePtr + iLogEntry );
	for ( int i = 0; i<MAX_LOGENTRIES; i++ )
	{
		if ( pLogEntry->m_dEntries[i]!=iIndex )
			continue;

		pLogEntry->m_dEntries[i] = pLogEntry->m_dEntries[pLogEntry->m_iUsed-1]; // RemoveFast
		pLogEntry->m_iUsed--;
		break;
	}
	assert ( pLogEntry->m_iUsed>=0 );

	// remove from tag entries list
	if ( pLogEntry->m_iUsed==0 )
	{
		if ( pTag->m_iLogHead==iLogEntry )
		{
			pTag->m_iLogHead = pLogEntry->m_iNext;
		} else
		{
			int iLog = pTag->m_iLogHead;
			while ( iLog>=0 )
			{
				AllocsLogEntry_t * pLog = (AllocsLogEntry_t*) ( m_pBasePtr + iLog );
				if ( iLogEntry!=pLog->m_iNext )
				{
					iLog = pLog->m_iNext;
					continue;
				} else
				{
					pLog->m_iNext = pLogEntry->m_iNext;
					break;
				}
			}
		}
		RawFree ( iLogEntry );
	}

	// update the tag descriptor
	pTag->m_iAllocs--;
	assert ( pTag->m_iAllocs>=0 );

	// remove the descriptor if its empty now
	if ( pTag->m_iAllocs==0 )
		RemoveTag ( pTag );
}


void CSphArena::TaggedFreeTag ( int iTag )
{
	if ( !m_iPages )
		return; // uninitialized

	assert ( iTag>=0 );
	CSphScopedLock<CSphProcessSharedMutex> tProcLock ( m_tProcMutex );
	CSphScopedLock<CSphMutex> tThdLock ( m_tThdMutex );

	// find that tag
	TagDesc_t * pTag = sphBinarySearch ( m_pTags, m_pTags+(*m_pTagCount)-1, bind ( &TagDesc_t::m_iTag ), iTag );
	if ( !pTag )
		return;

	// walk the log and free it
	int iLog = pTag->m_iLogHead;
	while ( iLog>=0 )
	{
		AllocsLogEntry_t * pLog = (AllocsLogEntry_t*) ( m_pBasePtr + iLog );
		iLog = pLog->m_iNext;

		// free each alloc if tag still matches
		for ( int i=0; i<pLog->m_iUsed; i++ )
		{
			int iIndex = pLog->m_dEntries[i];
			if ( m_pBasePtr[iIndex-1]==DWORD(iTag) )
			{
				m_pBasePtr[iIndex-1] = DWORD(-1); // avoid double free
				RawFree ( iIndex );
				pTag->m_iAllocs--;
			}
		}
	}

	// check for mismatches
	assert ( pTag->m_iAllocs==0 );

	// remove the descriptor
	RemoveTag ( pTag );
}

void CSphArena::ExamineTag ( tTester* pTest, int iTag )
{
	if ( !pTest )
		return;

	pTest->Reset();

	if ( !m_iPages )
		return; // uninitialized

	assert ( iTag>=0 );
	CSphScopedLock<CSphProcessSharedMutex> tProcLock ( m_tProcMutex );
	CSphScopedLock<CSphMutex> tThdLock ( m_tThdMutex );

	// find that tag
	TagDesc_t * pTag = sphBinarySearch ( m_pTags, m_pTags+(*m_pTagCount)-1, bind ( &TagDesc_t::m_iTag ), iTag );
	if ( !pTag )
		return;

	// walk the log and tick it's chunks
	int iLog = pTag->m_iLogHead;
	while ( iLog>=0 )
	{
		AllocsLogEntry_t * pLog = (AllocsLogEntry_t*) ( m_pBasePtr + iLog );
		iLog = pLog->m_iNext;

		// tick each alloc
		for ( int i=0; i<pLog->m_iUsed; i++ )
			pTest->TestData ( pLog->m_dEntries[i] );
	}
}

void CSphArena::RemoveTag ( TagDesc_t * pTag )
{
	assert ( pTag );
	assert ( pTag->m_iAllocs==0 );

	// dealloc log chain
	int iLog = pTag->m_iLogHead;
	while ( iLog>=0 )
	{
		AllocsLogEntry_t * pLog = (AllocsLogEntry_t*) ( m_pBasePtr + iLog );
		int iNext = pLog->m_iNext;

		RawFree ( iLog );
		iLog = iNext;
	}

	// remove tag from the list
	int iTail = m_pTags + (*m_pTagCount) - pTag - 1;
	memmove ( pTag, pTag+1, iTail*sizeof(TagDesc_t) );
	(*m_pTagCount)--;
}


#if ARENADEBUG
void CSphArena::CheckFreelists ()
{
	assert ( m_pFreelistHeads[0]==-1 || m_pPages[m_pFreelistHeads[0]].m_iSizeBits==0 );
	for ( int iSizeSlot=1; iSizeSlot<NUM_SIZES; iSizeSlot++ )
		assert ( m_pFreelistHeads[iSizeSlot]==-1 || m_pPages[m_pFreelistHeads[iSizeSlot]].m_iSizeBits-MIN_BITS+1==iSizeSlot );
}
#endif // ARENADEBUG

//////////////////////////////////////////////////////////////////////////

static CSphArena g_MvaArena; // global mega-arena

const char * sphArenaInit ( int iMaxBytes )
{
	if ( !g_pMvaArena )
		g_pMvaArena = g_MvaArena.ReInit ( iMaxBytes );

	const char * sError = g_MvaArena.GetError();
	return sError;
}


//////////////////////////////////////////////////////////////////////////

CSphMultiQueryArgs::CSphMultiQueryArgs ( const CSphVector<CSphFilterSettings> * pExtraFilters, int iIndexWeight )
	: m_pExtraFilters ( pExtraFilters )
	, m_iIndexWeight ( iIndexWeight )
	, m_iTag ( 0 )
	, m_bFactors ( false )
	, m_bLocalDF ( false )
	, m_pLocalDocs ( NULL )
	, m_iTotalDocs ( 0 )
{
	assert ( iIndexWeight>0 );
}


/////////////////////////////////////////////////////////////////////////////
// INDEX
/////////////////////////////////////////////////////////////////////////////

CSphIndex::CSphIndex ( const char * sIndexName, const char * sFilename )
	: m_iTID ( 0 )
	, m_bExpandKeywords ( false )
	, m_iExpansionLimit ( 0 )
	, m_tSchema ( sFilename )
	, m_bInplaceSettings ( false )
	, m_iHitGap ( 0 )
	, m_iDocinfoGap ( 0 )
	, m_fRelocFactor ( 0.0f )
	, m_fWriteFactor ( 0.0f )
	, m_bKeepFilesOpen ( false )
	, m_bPreloadWordlist ( true )
	, m_bBinlog ( true )
	, m_bStripperInited ( true )
	, m_bEnableStar ( true )
	, m_bId32to64 ( false )
	, m_pFieldFilter ( NULL )
	, m_pTokenizer ( NULL )
	, m_pQueryTokenizer ( NULL )
	, m_pDict ( NULL )
	, m_iMaxCachedDocs ( 0 )
	, m_iMaxCachedHits ( 0 )
	, m_sIndexName ( sIndexName )
	, m_sFilename ( sFilename )
{
}


CSphIndex::~CSphIndex ()
{
	SafeDelete ( m_pFieldFilter );
	SafeDelete ( m_pQueryTokenizer );
	SafeDelete ( m_pTokenizer );
	SafeDelete ( m_pDict );
}


void CSphIndex::SetInplaceSettings ( int iHitGap, int iDocinfoGap, float fRelocFactor, float fWriteFactor )
{
	m_iHitGap = iHitGap;
	m_iDocinfoGap = iDocinfoGap;
	m_fRelocFactor = fRelocFactor;
	m_fWriteFactor = fWriteFactor;
	m_bInplaceSettings = true;
}


void CSphIndex::SetFieldFilter ( ISphFieldFilter * pFieldFilter )
{
	if ( m_pFieldFilter!=pFieldFilter )
		SafeDelete ( m_pFieldFilter );
	m_pFieldFilter = pFieldFilter;
}


void CSphIndex::SetTokenizer ( ISphTokenizer * pTokenizer )
{
	if ( m_pTokenizer!=pTokenizer )
		SafeDelete ( m_pTokenizer );
	m_pTokenizer = pTokenizer;
}


void CSphIndex::SetupQueryTokenizer()
{
	// create and setup a master copy of query time tokenizer
	// that we can then use to create lightweight clones
	SafeDelete ( m_pQueryTokenizer );
	m_pQueryTokenizer = m_pTokenizer->Clone ( SPH_CLONE_QUERY );
	if ( IsStarDict() )
	{
		m_pQueryTokenizer->AddPlainChar ( '*' );
		m_pQueryTokenizer->AddPlainChar ( '?' );
		m_pQueryTokenizer->AddPlainChar ( '%' );
	}
	if ( m_tSettings.m_bIndexExactWords )
		m_pQueryTokenizer->AddPlainChar ( '=' );
	m_pQueryTokenizer->AddSpecials ( "()|-!@~\"/^$<" );
}


ISphTokenizer *	CSphIndex::LeakTokenizer ()
{
	ISphTokenizer * pTokenizer = m_pTokenizer;
	m_pTokenizer = NULL;
	return pTokenizer;
}


void CSphIndex::SetDictionary ( CSphDict * pDict )
{
	if ( m_pDict!=pDict )
		SafeDelete ( m_pDict );

	m_pDict = pDict;
}


CSphDict * CSphIndex::LeakDictionary ()
{
	CSphDict * pDict = m_pDict;
	m_pDict = NULL;
	return pDict;
}


void CSphIndex::Setup ( const CSphIndexSettings & tSettings )
{
	m_bStripperInited = true;
	m_tSettings = tSettings;
}


void CSphIndex::SetCacheSize ( int iMaxCachedDocs, int iMaxCachedHits )
{
	m_iMaxCachedDocs = iMaxCachedDocs;
	m_iMaxCachedHits = iMaxCachedHits;
}


float CSphIndex::GetGlobalIDF ( const CSphString & sWord, int64_t iDocsLocal, bool bPlainIDF ) const
{
	g_tGlobalIDFLock.Lock ();
	CSphGlobalIDF ** ppGlobalIDF = g_hGlobalIDFs ( m_sGlobalIDFPath );
	float fIDF = ppGlobalIDF && *ppGlobalIDF ? ( *ppGlobalIDF )->GetIDF ( sWord, iDocsLocal, bPlainIDF ) : 0.0f;
	g_tGlobalIDFLock.Unlock ();
	return fIDF;
}


bool CSphIndex::BuildDocList ( SphAttr_t ** ppDocList, int64_t * pCount, CSphString * ) const
{
	assert ( *ppDocList && pCount );
	*ppDocList = NULL;
	*pCount = 0;
	return true;
}


/////////////////////////////////////////////////////////////////////////////

CSphIndex * sphCreateIndexPhrase ( const char* szIndexName, const char * sFilename )
{
	return new CSphIndex_VLN ( szIndexName, sFilename );
}


CSphIndex_VLN::CSphIndex_VLN ( const char* sIndexName, const char * sFilename )
	: CSphIndex ( sIndexName, sFilename )
	, m_iLockFD ( -1 )
	, m_iTotalDups ( 0 )
	, m_dMinRow ( 0 )
	, m_dFieldLens ( SPH_MAX_FIELDS )
	, m_bKeepAttrs ( false )
{
	m_sFilename = sFilename;

	m_iDocinfo = 0;
	m_iDocinfoIndex = 0;
	m_pDocinfoIndex = NULL;

	m_bPreallocated = false;
	m_uVersion = INDEX_FORMAT_VERSION;

	m_iKillListSize = 0;
	m_uMinMaxIndex = 0;

	m_iIndexTag = -1;
	m_bIsEmpty = true;

	m_pPreread = NULL;
	m_pAttrsStatus = NULL;

	m_iMinDocid = 0;

	m_bOndiskAllAttr = false;
	m_bOndiskPoolAttr = false;

	ARRAY_FOREACH ( i, m_dFieldLens )
		m_dFieldLens[i] = 0;
}


CSphIndex_VLN::~CSphIndex_VLN ()
{
#if USE_WINDOWS
	if ( m_iIndexTag>=0 && g_pMvaArena )
#else
	if ( m_iIndexTag>=0 && g_bHeadProcess && g_pMvaArena )
#endif
		g_MvaArena.TaggedFreeTag ( m_iIndexTag );

#if !USE_WINDOWS
	if ( g_bHeadProcess )
#endif
	Unlock();
}


/////////////////////////////////////////////////////////////////////////////


int CSphIndex_VLN::UpdateAttributes ( const CSphAttrUpdate & tUpd, int iIndex, CSphString & sError, CSphString & sWarning )
{
	// check if we can
	if ( m_tSettings.m_eDocinfo!=SPH_DOCINFO_EXTERN )
	{
		sError.SetSprintf ( "docinfo=extern required for updates" );
		return -1;
	}
	if ( m_bOndiskAllAttr || m_bOndiskPoolAttr )
	{
		sError.SetSprintf ( "can not update ondisk_attrs enabled index" );
		return -1;
	}

	assert ( tUpd.m_dDocids.GetLength()==0 || tUpd.m_dRows.GetLength()==0 );
	DWORD uRows = Max ( tUpd.m_dDocids.GetLength(), tUpd.m_dRows.GetLength() );
	bool bRaw = tUpd.m_dDocids.GetLength()==0;

	// check if we have to
	assert ( (int)uRows==tUpd.m_dRowOffset.GetLength() );
	if ( !m_iDocinfo || !uRows )
		return 0;

	if ( m_bBinlog && g_pBinlog )
		g_pBinlog->BinlogUpdateAttributes ( &m_iTID, m_sIndexName.cstr(), tUpd );

	// remap update schema to index schema
	CSphVector<CSphAttrLocator> dLocators;
	CSphVector<bool> dFloats;
	CSphVector<bool> dBigints;
	CSphVector<bool> dDoubles;
	CSphVector<bool> dJsonFields;
	CSphVector<int64_t> dValues;
	CSphVector < CSphRefcountedPtr<ISphExpr> > dExpr;
	dLocators.Reserve ( tUpd.m_dAttrs.GetLength() );
	dFloats.Reserve ( tUpd.m_dAttrs.GetLength() );
	dBigints.Reserve ( tUpd.m_dAttrs.GetLength() ); // bigint flags for *source* schema.
	dDoubles.Reserve ( tUpd.m_dAttrs.GetLength() ); // double flags for *source* schema.
	dJsonFields.Reserve ( tUpd.m_dAttrs.GetLength() );
	dExpr.Resize ( tUpd.m_dAttrs.GetLength() );

	uint64_t uDst64 = 0;
	ARRAY_FOREACH ( i, tUpd.m_dAttrs )
	{
		int iIdx = m_tSchema.GetAttrIndex ( tUpd.m_dAttrs[i] );

		if ( iIdx<0 )
		{
			CSphString sJsonCol, sJsonKey;
			if ( sphJsonNameSplit ( tUpd.m_dAttrs[i], &sJsonCol, &sJsonKey ) )
			{
				iIdx = m_tSchema.GetAttrIndex ( sJsonCol.cstr() );
				if ( iIdx>=0 )
					dExpr[i] = sphExprParse ( tUpd.m_dAttrs[i], m_tSchema, NULL, NULL, sError, NULL );
			}
		}

		if ( iIdx>=0 )
		{
			// forbid updates on non-int columns
			const CSphColumnInfo & tCol = m_tSchema.GetAttr(iIdx);
			if ( !( tCol.m_eAttrType==SPH_ATTR_BOOL || tCol.m_eAttrType==SPH_ATTR_INTEGER || tCol.m_eAttrType==SPH_ATTR_TIMESTAMP
				|| tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET
				|| tCol.m_eAttrType==SPH_ATTR_BIGINT || tCol.m_eAttrType==SPH_ATTR_FLOAT || tCol.m_eAttrType==SPH_ATTR_JSON ))
			{
				sError.SetSprintf ( "attribute '%s' can not be updated "
					"(must be boolean, integer, bigint, float, timestamp, MVA or JSON)",
					tUpd.m_dAttrs[i] );
				return -1;
			}

			// forbid updates on MVA columns if there's no arena
			if ( ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET ) && !g_pMvaArena )
			{
				sError.SetSprintf ( "MVA attribute '%s' can not be updated (MVA arena not initialized)", tCol.m_sName.cstr() );
				return -1;
			}

			bool bSrcMva = ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET );
			bool bDstMva = ( tUpd.m_dTypes[i]==SPH_ATTR_UINT32SET || tUpd.m_dTypes[i]==SPH_ATTR_INT64SET );
			if ( bSrcMva!=bDstMva )
			{
				sError.SetSprintf ( "attribute '%s' MVA flag mismatch", tUpd.m_dAttrs[i] );
				return -1;
			}

			if ( tCol.m_eAttrType==SPH_ATTR_UINT32SET && tUpd.m_dTypes[i]==SPH_ATTR_INT64SET )
			{
				sError.SetSprintf ( "attribute '%s' MVA bits (dst=%d, src=%d) mismatch", tUpd.m_dAttrs[i],
					tCol.m_eAttrType, tUpd.m_dTypes[i] );
				return -1;
			}

			if ( tCol.m_eAttrType==SPH_ATTR_INT64SET )
				uDst64 |= ( U64C(1)<<i );

			dFloats.Add ( tCol.m_eAttrType==SPH_ATTR_FLOAT );
			dLocators.Add ( tCol.m_tLocator );
			dJsonFields.Add ( tCol.m_eAttrType==SPH_ATTR_JSON );

		} else if ( !tUpd.m_bIgnoreNonexistent )
		{
			sError.SetSprintf ( "attribute '%s' not found", tUpd.m_dAttrs[i] );
			return -1;
		}

		// this is a hack
		// Query parser tries to detect an attribute type. And this is wrong because, we should
		// take attribute type from schema. Probably we'll rewrite updates in future but
		// for now this fix just works.
		// Fixes cases like UPDATE float_attr=1 WHERE id=1;
		if ( tUpd.m_dTypes[i]==SPH_ATTR_INTEGER && m_tSchema.GetAttr(iIdx).m_eAttrType==SPH_ATTR_FLOAT )
		{
			const_cast<CSphAttrUpdate &>(tUpd).m_dTypes[i] = SPH_ATTR_FLOAT;
			const_cast<CSphAttrUpdate &>(tUpd).m_dPool[i] = sphF2DW ( (float)tUpd.m_dPool[i] );
		}

		dBigints.Add ( tUpd.m_dTypes[i]==SPH_ATTR_BIGINT );
		dDoubles.Add ( tUpd.m_dTypes[i]==SPH_ATTR_FLOAT );
		switch ( tUpd.m_dTypes[i] )
		{
		case SPH_ATTR_FLOAT:	dValues.Add ( sphD2QW ( (double)sphDW2F ( tUpd.m_dPool[i] ) ) ); break;
		case SPH_ATTR_BIGINT:	dValues.Add ( MVA_UPSIZE ( &tUpd.m_dPool[i] ) ); break;
		default:				dValues.Add ( tUpd.m_dPool[i] ); break;
		}
	}
	assert ( tUpd.m_bIgnoreNonexistent || ( dLocators.GetLength()==tUpd.m_dAttrs.GetLength() ) );

	// FIXME! FIXME! FIXME! overwriting just-freed blocks might hurt concurrent searchers;
	// should implement a simplistic MVCC-style delayed-free to avoid that

	// do the update
	const int iFirst = ( iIndex<0 ) ? 0 : iIndex;
	const int iLast = ( iIndex<0 ) ? uRows : iIndex+1;

	// first pass, if needed
	if ( tUpd.m_bStrict )
	{
		for ( int iUpd=iFirst; iUpd<iLast; iUpd++ )
		{
			DWORD * pEntry = const_cast < DWORD * > ( bRaw ? tUpd.m_dRows[iUpd] : FindDocinfo ( tUpd.m_dDocids[iUpd] ) );
			if ( !pEntry )
				continue; // no such id
			pEntry = DOCINFO2ATTRS(pEntry);
			int iPos = tUpd.m_dRowOffset[iUpd];
			ARRAY_FOREACH ( iCol, tUpd.m_dAttrs )
				if ( dJsonFields[iCol] )
				{
					ESphJsonType eType = dDoubles[iCol]
					? JSON_DOUBLE
					: ( dBigints[iCol] ? JSON_INT64 : JSON_INT32 );

					if ( !sphJsonInplaceUpdate ( eType, dValues[iCol], dExpr[iCol].Ptr(), m_tString.GetWritePtr(), pEntry, true ) )
					{
						sError.SetSprintf ( "attribute '%s' can not be updated (not found or incompatible types) ", tUpd.m_dAttrs[iCol] );
						return -1;
					}

					iPos += dBigints[iCol]?2:1;
				}
		}
	}

	// row update must leave it in cosistent state; so let's preallocate all the needed MVA
	// storage upfront to avoid suddenly having to rollback if allocation fails later
	int iNumMVA = 0;
	ARRAY_FOREACH ( i, tUpd.m_dAttrs )
		if ( tUpd.m_dTypes[i]==SPH_ATTR_UINT32SET || tUpd.m_dTypes[i]==SPH_ATTR_INT64SET )
			iNumMVA++;

	// OPTIMIZE! execute the code below conditionally
	CSphVector<DWORD*> dRowPtrs;
	CSphVector<int> dMvaPtrs;

	dRowPtrs.Resize ( uRows );
	dMvaPtrs.Resize ( uRows*iNumMVA );
	dMvaPtrs.Fill ( -1 );

	// preallocate
	bool bFailed = false;
	for ( int iUpd=iFirst; iUpd<iLast && !bFailed; iUpd++ )
	{
		dRowPtrs[iUpd] = const_cast < DWORD * > ( bRaw ? tUpd.m_dRows[iUpd] : FindDocinfo ( tUpd.m_dDocids[iUpd] ) );
		if ( !dRowPtrs[iUpd] )
			continue; // no such id

		int iPoolPos = tUpd.m_dRowOffset[iUpd];
		int iMvaPtr = iUpd*iNumMVA;
		ARRAY_FOREACH_COND ( iCol, tUpd.m_dAttrs, !bFailed )
		{
			bool bSrcMva32 = ( tUpd.m_dTypes[iCol]==SPH_ATTR_UINT32SET );
			bool bSrcMva64 = ( tUpd.m_dTypes[iCol]==SPH_ATTR_INT64SET );
			if (!( bSrcMva32 || bSrcMva64 )) // FIXME! optimize using a prebuilt dword mask?
			{
				iPoolPos++;
				if ( dBigints[iCol] )
					iPoolPos++;
				continue;
			}

			// get the requested new count
			int iNewCount = (int)tUpd.m_dPool[iPoolPos++];
			iPoolPos += iNewCount;

			// try to alloc
			int iAlloc = -1;
			if ( iNewCount )
			{
				bool bDst64 = ( uDst64 & ( U64C(1) << iCol ) )!=0;
				assert ( (iNewCount%2)==0 );
				int iLen = ( bDst64 ? iNewCount : iNewCount/2 );
				iAlloc = g_MvaArena.TaggedAlloc ( m_iIndexTag, (1+iLen)*sizeof(DWORD)+sizeof(SphDocID_t) );
				if ( iAlloc<0 )
					bFailed = true;
			}

			// whatever the outcome, move the pointer
			dMvaPtrs[iMvaPtr++] = iAlloc;
		}
	}

	// if there were any allocation failures, rollback everything
	if ( bFailed )
	{
		ARRAY_FOREACH ( i, dMvaPtrs )
			if ( dMvaPtrs[i]>=0 )
				g_MvaArena.TaggedFreeIndex ( m_iIndexTag, dMvaPtrs[i] );

		sError.SetSprintf ( "out of pool memory on MVA update" );
		return -1;
	}

	// preallocation went OK; do the actual update
	int iRowStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
	int iUpdated = 0;
	DWORD uUpdateMask = 0;
	int iJsonWarnings = 0;

	for ( int iUpd=iFirst; iUpd<iLast; iUpd++ )
	{
		bool bUpdated = false;

		DWORD * pEntry = dRowPtrs[iUpd];
		if ( !pEntry )
			continue; // no such id

		int64_t iBlock = int64_t ( pEntry-m_tAttr.GetWritePtr() ) / ( iRowStride*DOCINFO_INDEX_FREQ );
		DWORD * pBlockRanges = const_cast < DWORD * > ( &m_pDocinfoIndex[iBlock*iRowStride*2] );
		DWORD * pIndexRanges = const_cast < DWORD * > ( &m_pDocinfoIndex[m_iDocinfoIndex*iRowStride*2] );
		assert ( iBlock>=0 && iBlock<m_iDocinfoIndex );

		assert ( bRaw || ( DOCINFO2ID(pEntry)==tUpd.m_dDocids[iUpd] ) );
		pEntry = DOCINFO2ATTRS(pEntry);

		int iPos = tUpd.m_dRowOffset[iUpd];
		int iMvaPtr = iUpd*iNumMVA;
		ARRAY_FOREACH ( iCol, tUpd.m_dAttrs )
		{
			bool bSrcMva32 = ( tUpd.m_dTypes[iCol]==SPH_ATTR_UINT32SET );
			bool bSrcMva64 = ( tUpd.m_dTypes[iCol]==SPH_ATTR_INT64SET );
			bool bSrcJson = ( dJsonFields[iCol] );
			if (!( bSrcMva32 || bSrcMva64 || bSrcJson )) // FIXME! optimize using a prebuilt dword mask?
			{
				// plain update
				SphAttr_t uValue = dBigints[iCol] ? MVA_UPSIZE ( &tUpd.m_dPool[iPos] ) : tUpd.m_dPool[iPos];
				sphSetRowAttr ( pEntry, dLocators[iCol], uValue );

				// update block and index ranges
				for ( int i=0; i<2; i++ )
				{
					DWORD * pBlock = i ? pBlockRanges : pIndexRanges;
					SphAttr_t uMin = sphGetRowAttr ( DOCINFO2ATTRS ( pBlock ), dLocators[iCol] );
					SphAttr_t uMax = sphGetRowAttr ( DOCINFO2ATTRS ( pBlock+iRowStride ) , dLocators[iCol] );
					if ( dFloats[iCol] ) // update float's indexes assumes float comparision
					{
						float fValue = sphDW2F ( (DWORD) uValue );
						float fMin = sphDW2F ( (DWORD) uMin );
						float fMax = sphDW2F ( (DWORD) uMax );
						if ( fValue<fMin )
							sphSetRowAttr ( DOCINFO2ATTRS ( pBlock ), dLocators[iCol], sphF2DW ( fValue ) );
						if ( fValue>fMax )
							sphSetRowAttr ( DOCINFO2ATTRS ( pBlock+iRowStride ), dLocators[iCol], sphF2DW ( fValue ) );
					} else // update usual integers
					{
						if ( uValue<uMin )
							sphSetRowAttr ( DOCINFO2ATTRS ( pBlock ), dLocators[iCol], uValue );
						if ( uValue>uMax )
							sphSetRowAttr ( DOCINFO2ATTRS ( pBlock+iRowStride ), dLocators[iCol], uValue );
					}
				}

				bUpdated = true;
				uUpdateMask |= ATTRS_UPDATED;

				// next
				iPos += dBigints[iCol]?2:1;
				continue;
			}

			if ( bSrcJson )
			{
				ESphJsonType eType = dDoubles[iCol]
				? JSON_DOUBLE
				: ( dBigints[iCol] ? JSON_INT64 : JSON_INT32 );

				if ( sphJsonInplaceUpdate ( eType, dValues[iCol], dExpr[iCol].Ptr(), m_tString.GetWritePtr(), pEntry, true ) )
				{
					bUpdated = true;
					uUpdateMask |= ATTRS_STRINGS_UPDATED;

				} else
					iJsonWarnings++;

				iPos += dBigints[iCol]?2:1;
				continue;
			}

			// MVA update
			DWORD uOldIndex = MVA_DOWNSIZE ( sphGetRowAttr ( pEntry, dLocators[iCol] ) );

			// get new count, store new data if needed
			DWORD uNew = tUpd.m_dPool[iPos++];
			const DWORD * pSrc = tUpd.m_dPool.Begin() + iPos;
			iPos += uNew;

			int64_t iNewMin = LLONG_MAX, iNewMax = LLONG_MIN;
			int iNewIndex = dMvaPtrs[iMvaPtr++];
			if ( uNew )
			{
				assert ( iNewIndex>=0 );
				SphDocID_t* pDocid = (SphDocID_t *)(g_pMvaArena + iNewIndex);
				*pDocid++ = ( bRaw ? DOCINFO2ID ( tUpd.m_dRows[iUpd] ) : tUpd.m_dDocids[iUpd] );
				iNewIndex = (DWORD *)pDocid - g_pMvaArena;

				assert ( iNewIndex>=0 );
				DWORD * pDst = g_pMvaArena + iNewIndex;

				bool bDst64 = ( uDst64 & ( U64C(1) << iCol ) )!=0;
				assert ( ( uNew%2 )==0 );
				int iLen = ( bDst64 ? uNew : uNew/2 );
				// setup new value (flagged index) to store within row
				uNew = DWORD(iNewIndex) | MVA_ARENA_FLAG;

				// MVA values counter first
				*pDst++ = iLen;
				if ( bDst64 )
				{
					while ( iLen )
					{
						int64_t uValue = MVA_UPSIZE ( pSrc );
						iNewMin = Min ( iNewMin, uValue );
						iNewMax = Max ( iNewMax, uValue );
						*pDst++ = *pSrc++;
						*pDst++ = *pSrc++;
						iLen -= 2;
					}
				} else
				{
					while ( iLen-- )
					{
						DWORD uValue = *pSrc;
						pSrc += 2;
						*pDst++ = uValue;
						iNewMin = Min ( iNewMin, uValue );
						iNewMax = Max ( iNewMax, uValue );
					}
				}
			}

			// store new value
			sphSetRowAttr ( pEntry, dLocators[iCol], uNew );

			// update block and index ranges
			if ( uNew )
				for ( int i=0; i<2; i++ )
			{
				DWORD * pBlock = i ? pBlockRanges : pIndexRanges;
				int64_t iMin = sphGetRowAttr ( DOCINFO2ATTRS ( pBlock ), dLocators[iCol] );
				int64_t iMax = sphGetRowAttr ( DOCINFO2ATTRS ( pBlock+iRowStride ), dLocators[iCol] );
				if ( iNewMin<iMin || iNewMax>iMax )
				{
					sphSetRowAttr ( DOCINFO2ATTRS ( pBlock ), dLocators[iCol], Min ( iMin, iNewMin ) );
					sphSetRowAttr ( DOCINFO2ATTRS ( pBlock+iRowStride ), dLocators[iCol], Max ( iMax, iNewMax ) );
				}
			}

			// free old storage if needed
			if ( uOldIndex & MVA_ARENA_FLAG )
			{
				uOldIndex = ((DWORD*)((SphDocID_t*)(g_pMvaArena + (uOldIndex & MVA_OFFSET_MASK))-1))-g_pMvaArena;
				g_MvaArena.TaggedFreeIndex ( m_iIndexTag, uOldIndex );
			}

			bUpdated = true;
			uUpdateMask |= ATTRS_MVA_UPDATED;
		}

		if ( bUpdated )
			iUpdated++;
	}

	if ( iJsonWarnings>0 )
	{
		sWarning.SetSprintf ( "%d attribute(s) can not be updated (not found or incompatible types)", iJsonWarnings );
		if ( iUpdated==0 )
		{
			sError = sWarning;
			return -1;
		}
	}

	*m_pAttrsStatus |= uUpdateMask; // FIXME! add lock/atomic?
	return iUpdated;
}

bool CSphIndex_VLN::LoadPersistentMVA ( CSphString & sError )
{
	// prepare the file to load
	CSphAutoreader fdReader;
	if ( !fdReader.Open ( GetIndexFileName("mvp"), m_sLastError ) )
	{
		// no mvp means no saved attributes.
		m_sLastError = "";
		return true;
	}

	// check if we can
	if ( m_tSettings.m_eDocinfo!=SPH_DOCINFO_EXTERN )
	{
		sError.SetSprintf ( "docinfo=extern required for updates" );
		return false;
	}

	DWORD uDocs = fdReader.GetDword();

	// if we have docs to update
	if ( !uDocs )
		return false;

	CSphVector<SphDocID_t> dAffected ( uDocs );
	fdReader.GetBytes ( &dAffected[0], uDocs*sizeof(SphDocID_t) );

	// collect the indexes of MVA schema attributes
	CSphVector<CSphAttrLocator> dMvaLocators;
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
		if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET )
			dMvaLocators.Add ( tAttr.m_tLocator );
	}
#ifndef NDEBUG
	int iMva64 = dMvaLocators.GetLength();
#endif
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
		if ( tAttr.m_eAttrType==SPH_ATTR_INT64SET )
			dMvaLocators.Add ( tAttr.m_tLocator );
	}
	assert ( dMvaLocators.GetLength()!=0 );

	if ( g_MvaArena.GetError() ) // have to reset affected MVA in case of ( persistent MVA + no MVA arena )
	{
		ARRAY_FOREACH ( iDoc, dAffected )
		{
			DWORD * pDocinfo = const_cast<DWORD*> ( FindDocinfo ( dAffected[iDoc] ) );
			assert ( pDocinfo );
			DWORD * pAttrs = DOCINFO2ATTRS ( pDocinfo );
			ARRAY_FOREACH ( iMva, dMvaLocators )
			{
				// reset MVA from arena
				if ( MVA_DOWNSIZE ( sphGetRowAttr ( pAttrs, dMvaLocators[iMva] ) ) & MVA_ARENA_FLAG )
					sphSetRowAttr ( pAttrs, dMvaLocators[iMva], 0 );
			}
		}

		sphWarning ( "index '%s' forced to reset persistent MVAs ( %s )", m_sIndexName.cstr(), g_MvaArena.GetError() );
		fdReader.Close();
		return true;
	}

	CSphVector<DWORD*> dRowPtrs ( uDocs );
	CSphVector<int> dAllocs;
	dAllocs.Reserve ( uDocs );

	// prealloc values (and also preload)
	bool bFailed = false;
	ARRAY_FOREACH ( i, dAffected )
	{
		DWORD* pDocinfo = const_cast<DWORD*> ( FindDocinfo ( dAffected[i] ) );
		assert ( pDocinfo );
		pDocinfo = DOCINFO2ATTRS ( pDocinfo );
		ARRAY_FOREACH_COND ( j, dMvaLocators, !bFailed )
		{
			// if this MVA was updated
			if ( MVA_DOWNSIZE ( sphGetRowAttr ( pDocinfo, dMvaLocators[j] ) ) & MVA_ARENA_FLAG )
			{
				DWORD uCount = fdReader.GetDword();
				if ( uCount )
				{
					assert ( j<iMva64 || ( uCount%2 )==0 );
					int iAlloc = g_MvaArena.TaggedAlloc ( m_iIndexTag, (1+uCount)*sizeof(DWORD)+sizeof(SphDocID_t) );
					if ( iAlloc<0 )
						bFailed = true;
					else
					{
						SphDocID_t *pDocid = (SphDocID_t*)(g_pMvaArena + iAlloc);
						*pDocid++ = dAffected[i];
						DWORD * pData = (DWORD*)pDocid;
						*pData++ = uCount;
						fdReader.GetBytes ( pData, uCount*sizeof(DWORD) );
						dAllocs.Add ( iAlloc );
					}
				}
			}
		}
		if ( bFailed )
			break;
		dRowPtrs[i] = pDocinfo;
	}
	fdReader.Close();

	if ( bFailed )
	{
		ARRAY_FOREACH ( i, dAllocs )
			g_MvaArena.TaggedFreeIndex ( m_iIndexTag, dAllocs[i] );

		sError.SetSprintf ( "out of pool memory on loading persistent MVA values" );
		return false;
	}

	// prealloc && load ok, fix the attributes now
	int iAllocIndex = 0;
	ARRAY_FOREACH ( i, dAffected )
	{
		DWORD* pDocinfo = dRowPtrs[i];
		assert ( pDocinfo );
		ARRAY_FOREACH_COND ( j, dMvaLocators, !bFailed )
			// if this MVA was updated
			if ( MVA_DOWNSIZE ( sphGetRowAttr ( pDocinfo, dMvaLocators[j] ) ) & MVA_ARENA_FLAG )
				sphSetRowAttr ( pDocinfo, dMvaLocators[j],
					((DWORD*)(((SphDocID_t*)(g_pMvaArena + dAllocs[iAllocIndex++]))+1) - g_pMvaArena) | MVA_ARENA_FLAG );
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////

bool CSphIndex_VLN::PrecomputeMinMax()
{
	if ( !m_iDocinfo )
		return true;

	AttrIndexBuilder_c tBuilder ( m_tSchema );
	tBuilder.Prepare ( m_pDocinfoIndex, m_pDocinfoIndex + ( m_iDocinfoIndex+1 ) * 2 * ( DOCINFO_IDSIZE + m_tSchema.GetRowSize() ) );

	int iStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
	m_tProgress.m_ePhase = CSphIndexProgress::PHASE_PRECOMPUTE;
	m_tProgress.m_iDone = 0;
	m_uMinMaxIndex = 0;

	for ( int64_t iIndexEntry=0; iIndexEntry<m_iDocinfo; iIndexEntry++ )
	{
		if ( !tBuilder.Collect ( m_tAttr.GetWritePtr() + iIndexEntry * iStride, m_tMva.GetWritePtr(),
				m_tMva.GetNumEntries(), m_sLastError, true ) )
				return false;

		m_uMinMaxIndex += iStride;

		// show progress
		int64_t iDone = (iIndexEntry+1)*1000/m_iDocinfoIndex;
		if ( iDone!=m_tProgress.m_iDone )
		{
			m_tProgress.m_iDone = (int)iDone;
			m_tProgress.Show ( m_tProgress.m_iDone==1000 );
		}
	}

	tBuilder.FinishCollect();
	return true;
}

// safely rename an index file
bool CSphIndex_VLN::JuggleFile ( const char* szExt, CSphString & sError, bool bNeedOrigin ) const
{
	CSphString sExt = GetIndexFileName ( szExt );
	CSphString sExtNew, sExtOld;
	sExtNew.SetSprintf ( "%s.tmpnew", sExt.cstr() );
	sExtOld.SetSprintf ( "%s.tmpold", sExt.cstr() );

	if ( ::rename ( sExt.cstr(), sExtOld.cstr() ) )
	{
		if ( bNeedOrigin )
		{
			sError.SetSprintf ( "rename '%s' to '%s' failed: %s", sExt.cstr(), sExtOld.cstr(), strerror(errno) );
			return false;
		}
	}

	if ( ::rename ( sExtNew.cstr(), sExt.cstr() ) )
	{
		if ( bNeedOrigin && !::rename ( sExtOld.cstr(), sExt.cstr() ) )
		{
			// rollback failed too!
			sError.SetSprintf ( "rollback rename to '%s' failed: %s; INDEX UNUSABLE; FIX FILE NAMES MANUALLY", sExt.cstr(), strerror(errno) );
		} else
		{
			// rollback went ok
			sError.SetSprintf ( "rename '%s' to '%s' failed: %s", sExtNew.cstr(), sExt.cstr(), strerror(errno) );
		}
		return false;
	}

	// all done
	::unlink ( sExtOld.cstr() );
	return true;
}

bool CSphIndex_VLN::SaveAttributes ( CSphString & sError ) const
{
	if ( !m_pAttrsStatus || !*m_pAttrsStatus || !m_iDocinfo )
		return true;

	if ( m_bOndiskAllAttr || m_bOndiskPoolAttr )
	{
		sError.SetSprintf ( "ondisk_attrs enabled; saving is not (yet) possible" );
		return false;
	}

	DWORD uAttrStatus = *m_pAttrsStatus;

	sphLogDebugvv ( "index '%s' attrs (%d) saving...", m_sIndexName.cstr(), uAttrStatus );

	assert ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN && m_iDocinfo && m_tAttr.GetWritePtr() );

	for ( ; uAttrStatus & ATTRS_MVA_UPDATED ; )
	{
		// collect the indexes of MVA schema attributes
		CSphVector<CSphAttrLocator> dMvaLocators;
		for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
		{
			const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
			if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET )
				dMvaLocators.Add ( tAttr.m_tLocator );
		}
#ifndef NDEBUG
		int iMva64 = dMvaLocators.GetLength();
#endif
		for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
		{
			const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
			if ( tAttr.m_eAttrType==SPH_ATTR_INT64SET )
				dMvaLocators.Add ( tAttr.m_tLocator );
		}
		assert ( dMvaLocators.GetLength()!=0 );

		// collect the list of all docids with changed MVA attributes
		CSphVector<SphDocID_t> dAffected;
		{
			tDocCollector dCollect ( dAffected );
			g_MvaArena.ExamineTag ( &dCollect, m_iIndexTag );
		}
		dAffected.Uniq();

		if ( !dAffected.GetLength() )
			break;

		// prepare the file to save into;
		CSphWriter fdFlushMVA;
		fdFlushMVA.OpenFile ( GetIndexFileName("mvp.tmpnew"), sError );
		if ( fdFlushMVA.IsError() )
			return false;

		// save the vector of affected docids
		DWORD uPos = dAffected.GetLength();
		fdFlushMVA.PutDword ( uPos );
		fdFlushMVA.PutBytes ( &dAffected[0], uPos*sizeof(SphDocID_t) );

		// save the updated MVA vectors
		ARRAY_FOREACH ( i, dAffected )
		{
			DWORD* pDocinfo = const_cast<DWORD*> ( FindDocinfo ( dAffected[i] ) );
			assert ( pDocinfo );

			pDocinfo = DOCINFO2ATTRS ( pDocinfo );
			ARRAY_FOREACH ( j, dMvaLocators )
			{
				DWORD uOldIndex = MVA_DOWNSIZE ( sphGetRowAttr ( pDocinfo, dMvaLocators[j] ) );
				// if this MVA was updated
				if ( uOldIndex & MVA_ARENA_FLAG )
				{
					DWORD * pMva = g_pMvaArena + ( uOldIndex & MVA_OFFSET_MASK );
					DWORD uCount = *pMva;
					assert ( j<iMva64 || ( uCount%2 )==0 );
					fdFlushMVA.PutDword ( uCount );
					fdFlushMVA.PutBytes ( pMva+1, uCount*sizeof(DWORD) );
				}
			}
		}
		fdFlushMVA.CloseFile();
		if ( !JuggleFile ( "mvp", sError, false ) )
			return false;
		break;
	}

	if ( m_bId32to64 )
	{
		sError.SetSprintf ( "id32 index loaded by id64 binary; saving is not (yet) possible" );
		return false;
	}

	assert ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN && m_iDocinfo && m_tAttr.GetWritePtr() );

	// save current state
	CSphAutofile fdTmpnew ( GetIndexFileName("spa.tmpnew"), SPH_O_NEW, sError );
	if ( fdTmpnew.GetFD()<0 )
		return false;

	int uStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
	int64_t iSize = m_iDocinfo*sizeof(DWORD)*uStride;
	if ( m_uVersion>=20 )
		iSize += (m_iDocinfoIndex+1)*uStride*sizeof(CSphRowitem)*2;

	if ( !sphWriteThrottled ( fdTmpnew.GetFD(), m_tAttr.GetWritePtr(), iSize, "docinfo", sError, &g_tThrottle ) )
		return false;

	fdTmpnew.Close ();

	if ( !JuggleFile ( "spa", sError ) )
		return false;

	if ( m_bBinlog && g_pBinlog )
		g_pBinlog->NotifyIndexFlush ( m_sIndexName.cstr(), m_iTID, false );

	// save .sps file (inplace update only, no remapping/resizing)
	if ( uAttrStatus & ATTRS_STRINGS_UPDATED )
	{
		CSphWriter tStrWriter;
		if ( !tStrWriter.OpenFile ( GetIndexFileName("sps.tmpnew"), sError ) )
			return false;
		tStrWriter.PutBytes ( m_tString.GetWritePtr(), m_tString.GetLengthBytes() );
		tStrWriter.CloseFile();
		if ( !JuggleFile ( "sps", sError ) )
			return false;
	}

	if ( *m_pAttrsStatus==uAttrStatus )
		*m_pAttrsStatus = 0;

	sphLogDebugvv ( "index '%s' attrs (%d) saved", m_sIndexName.cstr(), *m_pAttrsStatus );

	return true;
}

DWORD CSphIndex_VLN::GetAttributeStatus () const
{
	assert ( m_pAttrsStatus );
	return *m_pAttrsStatus;
}

const CSphRowitem * CSphIndex_VLN::CopyRow ( const CSphRowitem * pDocinfo, DWORD * pTmpDocinfo, const CSphColumnInfo * pNewAttr, int iOldStride ) const
{
	SphDocID_t tDocId = DOCINFO2ID ( pDocinfo );
	const DWORD * pAttrs = DOCINFO2ATTRS ( pDocinfo );
	memcpy ( DOCINFO2ATTRS ( pTmpDocinfo ), pAttrs, (iOldStride - DOCINFO_IDSIZE)*sizeof(DWORD) );
	sphSetRowAttr ( DOCINFO2ATTRS ( pTmpDocinfo ), pNewAttr->m_tLocator, 0 );
	DOCINFOSETID ( pTmpDocinfo, tDocId );
	return pDocinfo + iOldStride;
}

bool CSphIndex_VLN::CreateFilesWithAttr ( int iPos, const CSphString & sAttrName, ESphAttr eAttrType, CSphString & sError )
{
	if ( m_tSchema.GetAttr ( sAttrName.cstr() ) )
	{
		sError.SetSprintf ( "'%s' attribute already in schema", sAttrName.cstr() );
		return false;
	}

	if ( m_bOndiskAllAttr || m_bOndiskPoolAttr )
	{
		sError.SetSprintf ( "ondisk_attrs enabled; adding attribute is not (yet) possible" );
		return false;
	}

	CSphSchema tNewSchema = m_tSchema;
	CSphColumnInfo tInfo ( sAttrName.cstr(), eAttrType );
	tNewSchema.InsertAttr ( iPos, tInfo, false );

	CSphFixedVector<CSphRowitem> dMinRow ( tNewSchema.GetRowSize() );
	int iOldStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
	int iNewStride = DOCINFO_IDSIZE + tNewSchema.GetRowSize();

	int64_t uNewMinMaxIndex = m_iDocinfo*iNewStride;

	BuildHeader_t tBuildHeader ( m_tStats );
	tBuildHeader.m_sHeaderExtension = "new.sph";
	tBuildHeader.m_pThrottle = &g_tThrottle;
	tBuildHeader.m_pMinRow = dMinRow.Begin();
	tBuildHeader.m_iMinDocid = m_iMinDocid;
	tBuildHeader.m_iKillListSize = m_iKillListSize;
	tBuildHeader.m_uMinMaxIndex = uNewMinMaxIndex;

	*(DictHeader_t*)&tBuildHeader = *(DictHeader_t*)&m_tWordlist;

	CSphSchema tOldSchema = m_tSchema;
	m_tSchema = tNewSchema;

	// save the header
	bool bBuildRes = BuildDone ( tBuildHeader, sError );
	m_tSchema = tOldSchema;
	if ( !bBuildRes )
		return false;

	// generate a new .SPA file
	CSphWriter tSPAWriter;
	tSPAWriter.SetBufferSize ( 524288 );
	CSphString sSPAfile = GetIndexFileName ( "new.spa" );
	if ( !tSPAWriter.OpenFile ( sSPAfile, sError ) )
		return false;

	const CSphRowitem * pDocinfo = m_tAttr.GetWritePtr();
	if ( !pDocinfo )
	{
		sError = "index must have at least one attribute";
		return false;
	}

	CSphFixedVector<DWORD> dTmpDocinfos ( iNewStride );
	DWORD * pTmpDocinfo = dTmpDocinfos.Begin();

	const CSphColumnInfo * pNewAttr = tNewSchema.GetAttr ( sAttrName.cstr() );
	assert ( pNewAttr );

	for ( int i = 0; i < m_iDocinfo + (m_iDocinfoIndex+1)*2 && !tSPAWriter.IsError(); i++ )
	{
		pDocinfo = CopyRow ( pDocinfo, pTmpDocinfo, pNewAttr, iOldStride );
		tSPAWriter.PutBytes ( pTmpDocinfo, iNewStride*sizeof(DWORD) );
	}

	if ( tSPAWriter.IsError() )
	{
		sError.SetSprintf ( "error writing to %s", sSPAfile.cstr() );
		return false;
	}

	return true;
}


bool CSphIndex_VLN::AddAttribute ( const CSphString & sAttrName, ESphAttr eAttrType, CSphString & sError )
{
	if ( m_tSchema.GetAttr ( sAttrName.cstr() ) )
	{
		sError.SetSprintf ( "'%s' attribute already in schema", sAttrName.cstr() );
		return false;
	}

	if ( m_bOndiskAllAttr || m_bOndiskPoolAttr )
	{
		sError.SetSprintf ( "ondisk_attrs enabled; adding attribute is not (yet) possible" );
		return false;
	}

	int iOldStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();

	CSphColumnInfo tInfo ( sAttrName.cstr(), eAttrType );
	int iPos = m_tSchema.GetAttrsCount();
	if ( m_tSettings.m_bIndexFieldLens )
		iPos -= m_tSchema.m_dFields.GetLength();
	m_tSchema.InsertAttr ( iPos, tInfo, false );

	int iNewStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();

	m_uMinMaxIndex = m_iDocinfo*iNewStride;
	if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_NONE )
		m_tSettings.m_eDocinfo = SPH_DOCINFO_EXTERN;

	const CSphColumnInfo * pNewAttr = m_tSchema.GetAttr ( sAttrName.cstr() );
	assert ( pNewAttr );

	CSphString sWarning;
	const CSphRowitem * pDocinfo = m_dAttrShared.GetWritePtr();
	CSphSharedBuffer<DWORD> pNewDocinfo;

	// fixme: this could cause inconsistency between on-disk and in-memory data
	if ( !pNewDocinfo.Alloc ( m_iDocinfo*iNewStride + (m_iDocinfoIndex+1)*iNewStride*2, sError, sWarning ) )
		return false;

	DWORD * pNewDocinfos = pNewDocinfo.GetWritePtr();
	assert ( pNewDocinfos );

	for ( int i = 0; i < m_iDocinfo + (m_iDocinfoIndex+1)*2; i++ )
	{
		pDocinfo = CopyRow ( pDocinfo, pNewDocinfos, pNewAttr, iOldStride );
		pNewDocinfos += iNewStride;
	}

	m_dAttrShared.Swap ( pNewDocinfo );
	m_tAttr.Set ( m_dAttrShared.GetWritePtr(), m_dAttrShared.GetNumEntries() );

	m_pDocinfoIndex = m_dAttrShared.GetWritePtr() + m_iDocinfo*iNewStride;

	return true;
}


/////////////////////////////////////////////////////////////////////////////

#define SPH_CMPHIT_LESS(a,b) \
	( a.m_iWordID<b.m_iWordID || \
	( a.m_iWordID==b.m_iWordID && a.m_iDocID<b.m_iDocID ) || \
	( a.m_iWordID==b.m_iWordID && a.m_iDocID==b.m_iDocID && a.m_iWordPos<b.m_iWordPos ) )


struct CmpHit_fn
{
	inline bool IsLess ( const CSphWordHit & a, const CSphWordHit & b ) const
	{
		return SPH_CMPHIT_LESS ( a, b );
	}
};


/// sort baked docinfos by document ID
struct DocinfoSort_fn
{
	typedef SphDocID_t MEDIAN_TYPE;

	int m_iStride;

	explicit DocinfoSort_fn ( int iStride )
		: m_iStride ( iStride )
	{}

	SphDocID_t Key ( DWORD * pData ) const
	{
		return DOCINFO2ID(pData);
	}

	void CopyKey ( SphDocID_t * pMed, DWORD * pVal ) const
	{
		*pMed = Key(pVal);
	}

	bool IsLess ( SphDocID_t a, SphDocID_t b ) const
	{
		return a < b;
	}

	void Swap ( DWORD * a, DWORD * b ) const
	{
		for ( int i=0; i<m_iStride; i++ )
			::Swap ( a[i], b[i] );
	}

	DWORD * Add ( DWORD * p, int i ) const
	{
		return p+i*m_iStride;
	}

	int Sub ( DWORD * b, DWORD * a ) const
	{
		return (int)((b-a)/m_iStride);
	}
};


void sphSortDocinfos ( DWORD * pBuf, int iCount, int iStride )
{
	DocinfoSort_fn fnSort ( iStride );
	sphSort ( pBuf, iCount, fnSort, fnSort );
}


CSphString CSphIndex_VLN::GetIndexFileName ( const char * sExt ) const
{
	CSphString sRes;
	sRes.SetSprintf ( "%s.%s", m_sFilename.cstr(), sExt );
	return sRes;
}


class CSphHitBuilder
{
public:
	CSphHitBuilder ( const CSphIndexSettings & tSettings, const CSphVector<SphWordID_t> & dHitless, bool bMerging, int iBufSize, CSphDict * pDict, CSphString * sError );
	~CSphHitBuilder () {}

	bool	CreateIndexFiles ( const char * sDocName, const char * sHitName, const char * sSkipName, bool bInplace, int iWriteBuffer, CSphAutofile & tHit, SphOffset_t * pSharedOffset );
	void	HitReset ();
	void	DoclistBeginEntry ( SphDocID_t uDocid, const DWORD * pAttrs );
	void	DoclistEndEntry ( Hitpos_t uLastPos );
	void	DoclistEndList ();
	void	cidxHit ( CSphAggregateHit * pHit, const CSphRowitem * pAttrs );
	bool	cidxDone ( int iMemLimit, int iMinInfixLen, int iMaxCodepointLen, DictHeader_t * pDictHeader );
	int		cidxWriteRawVLB ( int fd, CSphWordHit * pHit, int iHits, DWORD * pDocinfo, int iDocinfos, int iStride );

	SphOffset_t		GetHitfilePos () const { return m_wrHitlist.GetPos (); }
	void			CloseHitlist () { m_wrHitlist.CloseFile (); }
	bool			IsError () const { return ( m_pDict->DictIsError() || m_wrDoclist.IsError() || m_wrHitlist.IsError() ); }
	void			SetMin ( const CSphRowitem * pDynamic, int iDynamic );
	void			HitblockBegin () { m_pDict->HitblockBegin(); }
	bool			IsWordDict () const { return m_pDict->GetSettings().m_bWordDict; }
	void			SetThrottle ( ThrottleState_t * pState ) { m_pThrottle = pState; }

private:

	CSphWriter					m_wrDoclist;			///< wordlist writer
	CSphWriter					m_wrHitlist;			///< hitlist writer
	CSphWriter					m_wrSkiplist;			///< skiplist writer
	CSphFixedVector<BYTE>		m_dWriteBuffer;			///< my write buffer (for temp files)
	ThrottleState_t *			m_pThrottle;

	CSphFixedVector<CSphRowitem>	m_dMinRow;

	CSphAggregateHit			m_tLastHit;				///< hitlist entry
	BYTE						m_sLastKeyword [ MAX_KEYWORD_BYTES ];

	const CSphVector<SphWordID_t> &	m_dHitlessWords;
	CSphDict *					m_pDict;
	CSphString *				m_pLastError;

	SphOffset_t					m_iLastHitlistPos;		///< doclist entry
	SphOffset_t					m_iLastHitlistDelta;	///< doclist entry
	FieldMask_t				m_dLastDocFields;		///< doclist entry
	DWORD						m_uLastDocHits;			///< doclist entry

	CSphDictEntry				m_tWord;				///< dictionary entry

	ESphHitFormat				m_eHitFormat;
	ESphHitless					m_eHitless;
	bool						m_bMerging;

	CSphVector<SkiplistEntry_t>	m_dSkiplist;
};


CSphHitBuilder::CSphHitBuilder ( const CSphIndexSettings & tSettings,
	const CSphVector<SphWordID_t> & dHitless, bool bMerging, int iBufSize,
	CSphDict * pDict, CSphString * sError )
	: m_dWriteBuffer ( iBufSize )
	, m_dMinRow ( 0 )
	, m_dHitlessWords ( dHitless )
	, m_pDict ( pDict )
	, m_pLastError ( sError )
	, m_eHitFormat ( tSettings.m_eHitFormat )
	, m_eHitless ( tSettings.m_eHitless )
	, m_bMerging ( bMerging )
{
	m_sLastKeyword[0] = '\0';
	HitReset();

	m_iLastHitlistPos = 0;
	m_iLastHitlistDelta = 0;
	m_dLastDocFields.UnsetAll();
	m_uLastDocHits = 0;

	m_tWord.m_iDoclistOffset = 0;
	m_tWord.m_iDocs = 0;
	m_tWord.m_iHits = 0;

	assert ( m_pDict );
	assert ( m_pLastError );

	m_pThrottle = &g_tThrottle;
}


void CSphHitBuilder::SetMin ( const CSphRowitem * pDynamic, int iDynamic )
{
	assert ( !iDynamic || pDynamic );

	m_dMinRow.Reset ( iDynamic );
	ARRAY_FOREACH ( i, m_dMinRow )
	{
		m_dMinRow[i] = pDynamic[i];
	}
}


bool CSphHitBuilder::CreateIndexFiles ( const char * sDocName, const char * sHitName, const char * sSkipName,
	bool bInplace, int iWriteBuffer, CSphAutofile & tHit, SphOffset_t * pSharedOffset )
{
	// doclist and hitlist files
	m_wrDoclist.CloseFile();
	m_wrHitlist.CloseFile();
	m_wrSkiplist.CloseFile();

	m_wrDoclist.SetBufferSize ( m_dWriteBuffer.GetLength() );
	m_wrHitlist.SetBufferSize ( bInplace ? iWriteBuffer : m_dWriteBuffer.GetLength() );
	m_wrDoclist.SetThrottle ( m_pThrottle );
	m_wrHitlist.SetThrottle ( m_pThrottle );

	if ( !m_wrDoclist.OpenFile ( sDocName, *m_pLastError ) )
		return false;

	if ( bInplace )
	{
		sphSeek ( tHit.GetFD(), 0, SEEK_SET );
		m_wrHitlist.SetFile ( tHit, pSharedOffset, *m_pLastError );
	} else
	{
		if ( !m_wrHitlist.OpenFile ( sHitName, *m_pLastError ) )
			return false;
	}

	if ( !m_wrSkiplist.OpenFile ( sSkipName, *m_pLastError ) )
		return false;

	// put dummy byte (otherwise offset would start from 0, first delta would be 0
	// and VLB encoding of offsets would fuckup)
	BYTE bDummy = 1;
	m_wrDoclist.PutBytes ( &bDummy, 1 );
	m_wrHitlist.PutBytes ( &bDummy, 1 );
	m_wrSkiplist.PutBytes ( &bDummy, 1 );
	return true;
}


void CSphHitBuilder::HitReset()
{
	m_tLastHit.m_iDocID = 0;
	m_tLastHit.m_iWordID = 0;
	m_tLastHit.m_iWordPos = EMPTY_HIT;
	m_tLastHit.m_sKeyword = m_sLastKeyword;
}


// doclist entry format
// (with the new and shiny "inline hit" format, that is)
//
// zint docid_delta
// zint[] inline_attrs
// zint doc_hits
// if doc_hits==1:
// 		zint field_pos
// 		zint field_no
// else:
// 		zint field_mask
// 		zint hlist_offset_delta
//
// so 4 bytes/doc minimum
// avg 4-6 bytes/doc according to our tests


void CSphHitBuilder::DoclistBeginEntry ( SphDocID_t uDocid, const DWORD * pAttrs )
{
	// build skiplist
	// that is, save decoder state and doclist position per every 128 documents
	if ( ( m_tWord.m_iDocs & ( SPH_SKIPLIST_BLOCK-1 ) )==0 )
	{
		SkiplistEntry_t & tBlock = m_dSkiplist.Add();
		tBlock.m_iBaseDocid = m_tLastHit.m_iDocID;
		tBlock.m_iOffset = m_wrDoclist.GetPos();
		tBlock.m_iBaseHitlistPos = m_iLastHitlistPos;
	}

	// begin doclist entry
	m_wrDoclist.ZipOffset ( uDocid - m_tLastHit.m_iDocID );
	assert ( !pAttrs || m_dMinRow.GetLength() );
	if ( pAttrs )
	{
		ARRAY_FOREACH ( i, m_dMinRow )
			m_wrDoclist.ZipInt ( pAttrs[i] - m_dMinRow[i] );
	}
}


void CSphHitBuilder::DoclistEndEntry ( Hitpos_t uLastPos )
{
	// end doclist entry
	if ( m_eHitFormat==SPH_HIT_FORMAT_INLINE )
	{
		bool bIgnoreHits =
			( m_eHitless==SPH_HITLESS_ALL ) ||
			( m_eHitless==SPH_HITLESS_SOME && ( m_tWord.m_iDocs & 0x80000000 ) );

		// inline the only hit into doclist (unless it is completely discarded)
		// and finish doclist entry
		m_wrDoclist.ZipInt ( m_uLastDocHits );
		if ( m_uLastDocHits==1 && !bIgnoreHits )
		{
			m_wrHitlist.SeekTo ( m_iLastHitlistPos );
			m_wrDoclist.ZipInt ( uLastPos & 0x7FFFFF );
			m_wrDoclist.ZipInt ( uLastPos >> 23 );
			m_iLastHitlistPos -= m_iLastHitlistDelta;
			assert ( m_iLastHitlistPos>=0 );

		} else
		{
			m_wrDoclist.ZipInt ( m_dLastDocFields.GetMask32() );
			m_wrDoclist.ZipOffset ( m_iLastHitlistDelta );
		}
	} else // plain format - finish doclist entry
	{
		assert ( m_eHitFormat==SPH_HIT_FORMAT_PLAIN );
		m_wrDoclist.ZipOffset ( m_iLastHitlistDelta );
		m_wrDoclist.ZipInt ( m_dLastDocFields.GetMask32() );
		m_wrDoclist.ZipInt ( m_uLastDocHits );
	}
	m_dLastDocFields.UnsetAll();
	m_uLastDocHits = 0;

	// update keyword stats
	m_tWord.m_iDocs++;
}


void CSphHitBuilder::DoclistEndList ()
{
	// emit eof marker
	m_wrDoclist.ZipInt ( 0 );

	// emit skiplist
	// OPTIMIZE? placing it after doclist means an extra seek on searching
	// however placing it before means some (longer) doclist data moves while indexing
	if ( m_tWord.m_iDocs>SPH_SKIPLIST_BLOCK )
	{
		assert ( m_dSkiplist.GetLength() );
		assert ( m_dSkiplist[0].m_iOffset==m_tWord.m_iDoclistOffset );
		assert ( m_dSkiplist[0].m_iBaseDocid==0 );
		assert ( m_dSkiplist[0].m_iBaseHitlistPos==0 );

		m_tWord.m_iSkiplistOffset = m_wrSkiplist.GetPos();

		// delta coding, but with a couple of skiplist specific tricks
		// 1) first entry is omitted, it gets reconstructed from dict itself
		// both base values are zero, and offset equals doclist offset
		// 2) docids are at least SKIPLIST_BLOCK apart
		// doclist entries are at least 4*SKIPLIST_BLOCK bytes apart
		// so we additionally subtract that to improve delta coding
		// 3) zero deltas are allowed and *not* used as any markers,
		// as we know the exact skiplist entry count anyway
		SkiplistEntry_t tLast = m_dSkiplist[0];
		for ( int i=1; i<m_dSkiplist.GetLength(); i++ )
		{
			const SkiplistEntry_t & t = m_dSkiplist[i];
			assert ( t.m_iBaseDocid - tLast.m_iBaseDocid>=SPH_SKIPLIST_BLOCK );
			assert ( t.m_iOffset - tLast.m_iOffset>=4*SPH_SKIPLIST_BLOCK );
			m_wrSkiplist.ZipOffset ( t.m_iBaseDocid - tLast.m_iBaseDocid - SPH_SKIPLIST_BLOCK );
			m_wrSkiplist.ZipOffset ( t.m_iOffset - tLast.m_iOffset - 4*SPH_SKIPLIST_BLOCK );
			m_wrSkiplist.ZipOffset ( t.m_iBaseHitlistPos - tLast.m_iBaseHitlistPos );
			tLast = t;
		}
	}

	// in any event, reset skiplist
	m_dSkiplist.Resize ( 0 );
}


void CSphHitBuilder::cidxHit ( CSphAggregateHit * pHit, const CSphRowitem * pAttrs )
{
	assert (
		( pHit->m_iWordID!=0 && pHit->m_iWordPos!=EMPTY_HIT && pHit->m_iDocID!=0 ) || // it's either ok hit
		( pHit->m_iWordID==0 && pHit->m_iWordPos==EMPTY_HIT ) ); // or "flush-hit"

	/////////////
	// next word
	/////////////

	bool bNextWord = ( m_tLastHit.m_iWordID!=pHit->m_iWordID ||
		( m_pDict->GetSettings().m_bWordDict && strcmp ( (char*)m_tLastHit.m_sKeyword, (char*)pHit->m_sKeyword ) ) ); // OPTIMIZE?
	bool bNextDoc = bNextWord || ( m_tLastHit.m_iDocID!=pHit->m_iDocID );

	if ( bNextDoc )
	{
		// finish hitlist, if any
		Hitpos_t uLastPos = m_tLastHit.m_iWordPos;
		if ( m_tLastHit.m_iWordPos!=EMPTY_HIT )
		{
			m_wrHitlist.ZipInt ( 0 );
			m_tLastHit.m_iWordPos = EMPTY_HIT;
		}

		// finish doclist entry, if any
		if ( m_tLastHit.m_iDocID )
			DoclistEndEntry ( uLastPos );
	}

	if ( bNextWord )
	{
		// finish doclist, if any
		if ( m_tLastHit.m_iDocID )
		{
			// emit end-of-doclist marker
			DoclistEndList ();

			// emit dict entry
			m_tWord.m_uWordID = m_tLastHit.m_iWordID;
			m_tWord.m_sKeyword = m_tLastHit.m_sKeyword;
			m_tWord.m_iDoclistLength = m_wrDoclist.GetPos() - m_tWord.m_iDoclistOffset;
			m_pDict->DictEntry ( m_tWord );

			// reset trackers
			m_tWord.m_iDocs = 0;
			m_tWord.m_iHits = 0;

			m_tLastHit.m_iDocID = 0;
			m_iLastHitlistPos = 0;
		}

		// flush wordlist, if this is the end
		if ( pHit->m_iWordPos==EMPTY_HIT )
		{
			m_pDict->DictEndEntries ( m_wrDoclist.GetPos() );
			return;
		}

		assert ( pHit->m_iWordID > m_tLastHit.m_iWordID
			|| ( m_pDict->GetSettings().m_bWordDict &&
				pHit->m_iWordID==m_tLastHit.m_iWordID && strcmp ( (char*)pHit->m_sKeyword, (char*)m_tLastHit.m_sKeyword )>0 )
			|| m_bMerging );
		m_tWord.m_iDoclistOffset = m_wrDoclist.GetPos();
		m_tLastHit.m_iWordID = pHit->m_iWordID;
		if ( m_pDict->GetSettings().m_bWordDict )
		{
			assert ( strlen ( (char *)pHit->m_sKeyword )<sizeof(m_sLastKeyword)-1 );
			strncpy ( (char*)m_tLastHit.m_sKeyword, (char*)pHit->m_sKeyword, sizeof(m_sLastKeyword) ); // OPTIMIZE?
		}
	}

	if ( bNextDoc )
	{
		// begin new doclist entry for new doc id
		assert ( pHit->m_iDocID>m_tLastHit.m_iDocID );
		assert ( m_wrHitlist.GetPos()>=m_iLastHitlistPos );

		DoclistBeginEntry ( pHit->m_iDocID, pAttrs );
		m_iLastHitlistDelta = m_wrHitlist.GetPos() - m_iLastHitlistPos;

		m_tLastHit.m_iDocID = pHit->m_iDocID;
		m_iLastHitlistPos = m_wrHitlist.GetPos();
	}

	///////////
	// the hit
	///////////

	if ( !pHit->m_dFieldMask.TestAll(false) ) // merge aggregate hits into the current hit
	{
		int iHitCount = pHit->GetAggrCount();
		assert ( m_eHitless );
		assert ( iHitCount );
		assert ( !pHit->m_dFieldMask.TestAll(false) );

		m_uLastDocHits += iHitCount;
		for ( int i=0; i<FieldMask_t::SIZE; i++ )
			m_dLastDocFields[i] |= pHit->m_dFieldMask[i];
		m_tWord.m_iHits += iHitCount;

		if ( m_eHitless==SPH_HITLESS_SOME )
			m_tWord.m_iDocs |= 0x80000000;

	} else // handle normal hits
	{
		// add hit delta
		if ( pHit->m_iWordPos==m_tLastHit.m_iWordPos )
			return;

		assert ( m_tLastHit.m_iWordPos < pHit->m_iWordPos );
		m_wrHitlist.ZipInt ( pHit->m_iWordPos - m_tLastHit.m_iWordPos );
		m_tLastHit.m_iWordPos = pHit->m_iWordPos;
		m_tWord.m_iHits++;

		// update matched fields mask
		m_dLastDocFields.Set ( HITMAN::GetField ( pHit->m_iWordPos ) );
		m_uLastDocHits++;
	}
}


static void ReadSchemaColumn ( CSphReader & rdInfo, CSphColumnInfo & tCol, DWORD uVersion )
{
	tCol.m_sName = rdInfo.GetString ();
	if ( tCol.m_sName.IsEmpty () )
		tCol.m_sName = "@emptyname";

	tCol.m_sName.ToLower ();
	tCol.m_eAttrType = (ESphAttr) rdInfo.GetDword (); // FIXME? check/fixup?

	if ( uVersion>=5 ) // m_uVersion for searching
	{
		rdInfo.GetDword (); // ignore rowitem
		tCol.m_tLocator.m_iBitOffset = rdInfo.GetDword ();
		tCol.m_tLocator.m_iBitCount = rdInfo.GetDword ();
	} else
	{
		tCol.m_tLocator.m_iBitOffset = -1;
		tCol.m_tLocator.m_iBitCount = -1;
	}

	if ( uVersion>=16 ) // m_uVersion for searching
		tCol.m_bPayload = ( rdInfo.GetByte()!=0 );

	// WARNING! max version used here must be in sync with RtIndex_t::Prealloc
}


void ReadSchema ( CSphReader & rdInfo, CSphSchema & m_tSchema, DWORD uVersion, bool bDynamic )
{
	m_tSchema.Reset ();

	m_tSchema.m_dFields.Resize ( rdInfo.GetDword() );
	ARRAY_FOREACH ( i, m_tSchema.m_dFields )
		ReadSchemaColumn ( rdInfo, m_tSchema.m_dFields[i], uVersion );

	int iNumAttrs = rdInfo.GetDword();

	for ( int i=0; i<iNumAttrs; i++ )
	{
		CSphColumnInfo tCol;
		ReadSchemaColumn ( rdInfo, tCol, uVersion );
		m_tSchema.AddAttr ( tCol, bDynamic );
	}
}


static void WriteSchemaColumn ( CSphWriter & fdInfo, const CSphColumnInfo & tCol )
{
	int iLen = strlen ( tCol.m_sName.cstr() );
	fdInfo.PutDword ( iLen );
	fdInfo.PutBytes ( tCol.m_sName.cstr(), iLen );

	ESphAttr eAttrType = tCol.m_eAttrType;
	if ( eAttrType==SPH_ATTR_WORDCOUNT )
		eAttrType = SPH_ATTR_INTEGER;
	fdInfo.PutDword ( eAttrType );

	fdInfo.PutDword ( tCol.m_tLocator.CalcRowitem() ); // for backwards compatibility
	fdInfo.PutDword ( tCol.m_tLocator.m_iBitOffset );
	fdInfo.PutDword ( tCol.m_tLocator.m_iBitCount );

	fdInfo.PutByte ( tCol.m_bPayload );
}


void WriteSchema ( CSphWriter & fdInfo, const CSphSchema & tSchema )
{
	// schema
	fdInfo.PutDword ( tSchema.m_dFields.GetLength() );
	ARRAY_FOREACH ( i, tSchema.m_dFields )
		WriteSchemaColumn ( fdInfo, tSchema.m_dFields[i] );

	fdInfo.PutDword ( tSchema.GetAttrsCount() );
	for ( int i=0; i<tSchema.GetAttrsCount(); i++ )
		WriteSchemaColumn ( fdInfo, tSchema.GetAttr(i) );
}


void SaveIndexSettings ( CSphWriter & tWriter, const CSphIndexSettings & tSettings )
{
	tWriter.PutDword ( tSettings.m_iMinPrefixLen );
	tWriter.PutDword ( tSettings.m_iMinInfixLen );
	tWriter.PutDword ( tSettings.m_iMaxSubstringLen );
	tWriter.PutByte ( tSettings.m_bHtmlStrip ? 1 : 0 );
	tWriter.PutString ( tSettings.m_sHtmlIndexAttrs.cstr () );
	tWriter.PutString ( tSettings.m_sHtmlRemoveElements.cstr () );
	tWriter.PutByte ( tSettings.m_bIndexExactWords ? 1 : 0 );
	tWriter.PutDword ( tSettings.m_eHitless );
	tWriter.PutDword ( tSettings.m_eHitFormat );
	tWriter.PutByte ( tSettings.m_bIndexSP );
	tWriter.PutString ( tSettings.m_sZones );
	tWriter.PutDword ( tSettings.m_iBoundaryStep );
	tWriter.PutDword ( tSettings.m_iStopwordStep );
	tWriter.PutDword ( tSettings.m_iOvershortStep );
	tWriter.PutDword ( tSettings.m_iEmbeddedLimit );
	tWriter.PutByte ( tSettings.m_eBigramIndex );
	tWriter.PutString ( tSettings.m_sBigramWords );
	tWriter.PutByte ( tSettings.m_bIndexFieldLens );
	tWriter.PutByte ( tSettings.m_eChineseRLP );
	tWriter.PutString ( tSettings.m_sRLPContext );
}


bool CSphIndex_VLN::WriteHeader ( const BuildHeader_t & tBuildHeader, CSphWriter & fdInfo ) const
{
	// version
	fdInfo.PutDword ( INDEX_MAGIC_HEADER );
	fdInfo.PutDword ( INDEX_FORMAT_VERSION );

	// bits
	fdInfo.PutDword ( USE_64BIT );

	// docinfo
	fdInfo.PutDword ( m_tSettings.m_eDocinfo );

	// schema
	WriteSchema ( fdInfo, m_tSchema );

	// min doc
	fdInfo.PutOffset ( tBuildHeader.m_iMinDocid ); // was dword in v.1
	if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE )
		fdInfo.PutBytes ( tBuildHeader.m_pMinRow, m_tSchema.GetRowSize()*sizeof(CSphRowitem) );

	// wordlist checkpoints
	fdInfo.PutOffset ( tBuildHeader.m_iDictCheckpointsOffset );
	fdInfo.PutDword ( tBuildHeader.m_iDictCheckpoints );
	fdInfo.PutByte ( tBuildHeader.m_iInfixCodepointBytes );
	fdInfo.PutDword ( tBuildHeader.m_iInfixBlocksOffset );
	fdInfo.PutDword ( tBuildHeader.m_iInfixBlocksWordsSize );

	// index stats
	fdInfo.PutDword ( (DWORD)tBuildHeader.m_iTotalDocuments ); // FIXME? we don't expect over 4G docs per just 1 local index
	fdInfo.PutOffset ( tBuildHeader.m_iTotalBytes );
	fdInfo.PutDword ( tBuildHeader.m_iTotalDups );

	// index settings
	SaveIndexSettings ( fdInfo, m_tSettings );

	// tokenizer info
	assert ( m_pTokenizer );
	SaveTokenizerSettings ( fdInfo, m_pTokenizer, m_tSettings.m_iEmbeddedLimit );

	// dictionary info
	assert ( m_pDict );
	SaveDictionarySettings ( fdInfo, m_pDict, false, m_tSettings.m_iEmbeddedLimit );

	fdInfo.PutDword ( tBuildHeader.m_iKillListSize );
	fdInfo.PutOffset ( tBuildHeader.m_uMinMaxIndex );

	// field filter info
	SaveFieldFilterSettings ( fdInfo, m_pFieldFilter );

	// average field lengths
	if ( m_tSettings.m_bIndexFieldLens )
		ARRAY_FOREACH ( i, m_tSchema.m_dFields )
			fdInfo.PutOffset ( m_dFieldLens[i] );

	return true;
}


bool CSphIndex_VLN::BuildDone ( const BuildHeader_t & tBuildHeader, CSphString & sError ) const
{
	CSphWriter fdInfo;
	fdInfo.SetThrottle ( tBuildHeader.m_pThrottle );
	fdInfo.OpenFile ( GetIndexFileName ( tBuildHeader.m_sHeaderExtension ), sError );
	if ( fdInfo.IsError() )
		return false;

	if ( !WriteHeader ( tBuildHeader, fdInfo ) )
		return false;

	// close header
	fdInfo.CloseFile ();
	return !fdInfo.IsError();
}


bool CSphHitBuilder::cidxDone ( int iMemLimit, int iMinInfixLen, int iMaxCodepointLen, DictHeader_t * pDictHeader )
{
	assert ( pDictHeader );

	// finalize dictionary
	// in dict=crc mode, just flushes wordlist checkpoints
	// in dict=keyword mode, also creates infix index, if needed

	if ( iMinInfixLen>0 && m_pDict->GetSettings().m_bWordDict )
		pDictHeader->m_iInfixCodepointBytes = iMaxCodepointLen;

	if ( !m_pDict->DictEnd ( pDictHeader, iMemLimit, *m_pLastError, m_pThrottle ) )
		return false;

	// close all data files
	m_wrDoclist.CloseFile ();
	m_wrHitlist.CloseFile ( true );
	return !IsError();
}


inline int encodeVLB ( BYTE * buf, DWORD v )
{
	register BYTE b;
	register int n = 0;

	do
	{
		b = (BYTE)(v & 0x7f);
		v >>= 7;
		if ( v )
			b |= 0x80;
		*buf++ = b;
		n++;
	} while ( v );
	return n;
}


inline int encodeVLB8 ( BYTE * buf, uint64_t v )
{
	register BYTE b;
	register int n = 0;

	do {
		b = (BYTE)(v & 0x7f);
		v >>= 7;
		if ( v )
			b |= 0x80;
		*buf++ = b;
		n++;
	} while ( v );
	return n;
}


inline int encodeKeyword ( BYTE * pBuf, const char * pKeyword )
{
	int iLen = strlen ( pKeyword ); // OPTIMIZE! remove this and memcpy and check if thats faster
	assert ( iLen>0 && iLen<128 ); // so that ReadVLB()

	*pBuf = (BYTE) iLen;
	memcpy ( pBuf+1, pKeyword, iLen );
	return 1+iLen;
}


int CSphHitBuilder::cidxWriteRawVLB ( int fd, CSphWordHit * pHit, int iHits, DWORD * pDocinfo, int iDocinfos, int iStride )
{
	assert ( pHit );
	assert ( iHits>0 );

	/////////////////////////////
	// do simple bitwise hashing
	/////////////////////////////

	static const int HBITS = 11;
	static const int HSIZE = ( 1 << HBITS );

	SphDocID_t iStartID = 0;
	int dHash [ HSIZE+1 ];
	int iShift = 0;

	if ( pDocinfo )
	{
		iStartID = DOCINFO2ID ( pDocinfo );
		int iBits = sphLog2 ( DOCINFO2ID ( pDocinfo + (iDocinfos-1)*iStride ) - iStartID );
		iShift = ( iBits<HBITS ) ? 0 : ( iBits-HBITS );

		#ifndef NDEBUG
		for ( int i=0; i<=HSIZE; i++ )
			dHash[i] = -1;
		#endif

		dHash[0] = 0;
		int iHashed = 0;
		for ( int i=0; i<iDocinfos; i++ )
		{
			int iHash = (int)( ( DOCINFO2ID ( pDocinfo+i*iStride ) - iStartID ) >> iShift );
			assert ( iHash>=0 && iHash<HSIZE );

			if ( iHash>iHashed )
			{
				dHash [ iHashed+1 ] = i-1; // right boundary for prev hash value
				dHash [ iHash ] = i; // left boundary for next hash value
				iHashed = iHash;
			}
		}
		dHash [ iHashed+1 ] = iDocinfos-1; // right boundary for last hash value
	}

	///////////////////////////////////////
	// encode through a small write buffer
	///////////////////////////////////////

	BYTE *pBuf, *maxP;
	int n = 0, w;
	SphWordID_t d1, l1 = 0;
	SphDocID_t d2, l2 = 0;
	DWORD d3, l3 = 0; // !COMMIT must be wide enough

	int iGap = Max ( 16*sizeof(DWORD) + iStride*sizeof(DWORD) + ( m_pDict->GetSettings().m_bWordDict ? MAX_KEYWORD_BYTES : 0 ), 128u );
	pBuf = m_dWriteBuffer.Begin();
	maxP = m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() - iGap;

	SphDocID_t iAttrID = 0; // current doc id
	DWORD * pAttrs = NULL; // current doc attrs

	// hit aggregation state
	DWORD uHitCount = 0;
	DWORD uHitFieldMask = 0;

	const int iPositionShift = m_eHitless==SPH_HITLESS_SOME ? 1 : 0;

	while ( iHits-- )
	{
		// find attributes by id
		if ( pDocinfo && iAttrID!=pHit->m_iDocID )
		{
			int iHash = (int)( ( pHit->m_iDocID - iStartID ) >> iShift );
			assert ( iHash>=0 && iHash<HSIZE );

			int iStart = dHash[iHash];
			int iEnd = dHash[iHash+1];

			if ( pHit->m_iDocID==DOCINFO2ID ( pDocinfo + iStart*iStride ) )
			{
				pAttrs = DOCINFO2ATTRS ( pDocinfo + iStart*iStride );

			} else if ( pHit->m_iDocID==DOCINFO2ID ( pDocinfo + iEnd*iStride ) )
			{
				pAttrs = DOCINFO2ATTRS ( pDocinfo + iEnd*iStride );

			} else
			{
				pAttrs = NULL;
				while ( iEnd-iStart>1 )
				{
					// check if nothing found
					if (
						pHit->m_iDocID < DOCINFO2ID ( pDocinfo + iStart*iStride ) ||
						pHit->m_iDocID > DOCINFO2ID ( pDocinfo + iEnd*iStride ) )
							break;
					assert ( pHit->m_iDocID > DOCINFO2ID ( pDocinfo + iStart*iStride ) );
					assert ( pHit->m_iDocID < DOCINFO2ID ( pDocinfo + iEnd*iStride ) );

					int iMid = iStart + (iEnd-iStart)/2;
					if ( pHit->m_iDocID==DOCINFO2ID ( pDocinfo + iMid*iStride ) )
					{
						pAttrs = DOCINFO2ATTRS ( pDocinfo + iMid*iStride );
						break;
					}
					if ( pHit->m_iDocID<DOCINFO2ID ( pDocinfo + iMid*iStride ) )
						iEnd = iMid;
					else
						iStart = iMid;
				}
			}

			if ( !pAttrs )
				sphDie ( "INTERNAL ERROR: failed to lookup attributes while saving collected hits" );
			assert ( DOCINFO2ID ( pAttrs - DOCINFO_IDSIZE )==pHit->m_iDocID );
			iAttrID = pHit->m_iDocID;
		}

		// calc deltas
		d1 = pHit->m_iWordID - l1;
		d2 = pHit->m_iDocID - l2;
		d3 = pHit->m_iWordPos - l3;

		// ignore duplicate hits
		if ( d1==0 && d2==0 && d3==0 ) // OPTIMIZE? check if ( 0==(d1|d2|d3) ) is faster
		{
			pHit++;
			continue;
		}

		// replacement of hit itself by field-end form
		if ( d1==0 && d2==0 && HITMAN::GetLCS ( pHit->m_iWordPos )==HITMAN::GetLCS ( l3 ) )
		{
			l3 = pHit->m_iWordPos;
			pHit++;
			continue;
		}


		// non-zero delta restarts all the fields after it
		// because their deltas might now be negative
		if ( d1 ) d2 = pHit->m_iDocID;
		if ( d2 ) d3 = pHit->m_iWordPos;

		// when we moved to the next word or document
		bool bFlushed = false;
		if ( d1 || d2 )
		{
			// flush previous aggregate hit
			if ( uHitCount )
			{
				// we either skip all hits or the high bit must be available for marking
				// failing that, we can't produce a consistent index
				assert ( m_eHitless!=SPH_HITLESS_NONE );
				assert ( m_eHitless==SPH_HITLESS_ALL || !( uHitCount & 0x80000000UL ) );

				if ( m_eHitless!=SPH_HITLESS_ALL )
					uHitCount = ( uHitCount << 1 ) | 1;
				pBuf += encodeVLB ( pBuf, uHitCount );
				pBuf += encodeVLB ( pBuf, uHitFieldMask );
				assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );

				uHitCount = 0;
				uHitFieldMask = 0;

				bFlushed = true;
			}

			// start aggregating if we're skipping all hits or this word is in a list of ignored words
			if ( ( m_eHitless==SPH_HITLESS_ALL ) ||
				( m_eHitless==SPH_HITLESS_SOME && m_dHitlessWords.BinarySearch ( pHit->m_iWordID ) ) )
			{
				uHitCount = 1;
				uHitFieldMask |= 1 << HITMAN::GetField ( pHit->m_iWordPos );
			}

		} else if ( uHitCount ) // next hit for the same word/doc pair, update state if we need it
		{
			uHitCount++;
			uHitFieldMask |= 1 << HITMAN::GetField ( pHit->m_iWordPos );
		}

		// encode enough restart markers
		if ( d1 ) pBuf += encodeVLB ( pBuf, 0 );
		if ( d2 && !bFlushed ) pBuf += encodeVLB ( pBuf, 0 );

		assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );

		// encode deltas
#if USE_64BIT
#define LOC_ENCODE encodeVLB8
#else
#define LOC_ENCODE encodeVLB
#endif

		// encode keyword
		if ( d1 )
		{
			if ( m_pDict->GetSettings().m_bWordDict )
				pBuf += encodeKeyword ( pBuf, m_pDict->HitblockGetKeyword ( pHit->m_iWordID ) ); // keyword itself in case of keywords dict
			else
				pBuf += LOC_ENCODE ( pBuf, d1 ); // delta in case of CRC dict

			assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );
		}

		// encode docid delta
		if ( d2 )
		{
			pBuf += LOC_ENCODE ( pBuf, d2 );
			assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );
		}

#undef LOC_ENCODE

		// encode attrs
		if ( d2 && pAttrs )
		{
			for ( int i=0; i<iStride-DOCINFO_IDSIZE; i++ )
			{
				pBuf += encodeVLB ( pBuf, pAttrs[i] );
				assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );
			}
		}

		assert ( d3 );
		if ( !uHitCount ) // encode position delta, unless accumulating hits
		{
			pBuf += encodeVLB ( pBuf, d3 << iPositionShift );
			assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );
		}

		// update current state
		l1 = pHit->m_iWordID;
		l2 = pHit->m_iDocID;
		l3 = pHit->m_iWordPos;

		pHit++;

		if ( pBuf>maxP )
		{
			w = (int)(pBuf - m_dWriteBuffer.Begin());
			assert ( w<m_dWriteBuffer.GetLength() );
			if ( !sphWriteThrottled ( fd, m_dWriteBuffer.Begin(), w, "raw_hits", *m_pLastError, m_pThrottle ) )
				return -1;
			n += w;
			pBuf = m_dWriteBuffer.Begin();
		}
	}

	// flush last aggregate
	if ( uHitCount )
	{
		assert ( m_eHitless!=SPH_HITLESS_NONE );
		assert ( m_eHitless==SPH_HITLESS_ALL || !( uHitCount & 0x80000000UL ) );

		if ( m_eHitless!=SPH_HITLESS_ALL )
			uHitCount = ( uHitCount << 1 ) | 1;
		pBuf += encodeVLB ( pBuf, uHitCount );
		pBuf += encodeVLB ( pBuf, uHitFieldMask );

		assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );
	}

	pBuf += encodeVLB ( pBuf, 0 );
	pBuf += encodeVLB ( pBuf, 0 );
	pBuf += encodeVLB ( pBuf, 0 );
	assert ( pBuf<m_dWriteBuffer.Begin() + m_dWriteBuffer.GetLength() );
	w = (int)(pBuf - m_dWriteBuffer.Begin());
	assert ( w<m_dWriteBuffer.GetLength() );
	if ( !sphWriteThrottled ( fd, m_dWriteBuffer.Begin(), w, "raw_hits", *m_pLastError, m_pThrottle ) )
		return -1;
	n += w;

	return n;
}

/////////////////////////////////////////////////////////////////////////////

// OPTIMIZE?
inline bool SPH_CMPAGGRHIT_LESS ( const CSphAggregateHit & a, const CSphAggregateHit & b )
{
	if ( a.m_iWordID < b.m_iWordID )
		return true;

	if ( a.m_iWordID > b.m_iWordID )
		return false;

	if ( a.m_sKeyword )
	{
		int iCmp = strcmp ( (char*)a.m_sKeyword, (char*)b.m_sKeyword ); // OPTIMIZE?
		if ( iCmp!=0 )
			return ( iCmp<0 );
	}

	return
		( a.m_iDocID < b.m_iDocID ) ||
		( a.m_iDocID==b.m_iDocID && a.m_iWordPos<b.m_iWordPos );
}


/// hit priority queue entry
struct CSphHitQueueEntry : public CSphAggregateHit
{
	int m_iBin;
};


/// hit priority queue
struct CSphHitQueue
{
public:
	CSphHitQueueEntry *		m_pData;
	int						m_iSize;
	int						m_iUsed;

public:
	/// create queue
	explicit CSphHitQueue ( int iSize )
	{
		assert ( iSize>0 );
		m_iSize = iSize;
		m_iUsed = 0;
		m_pData = new CSphHitQueueEntry [ iSize ];
	}

	/// destroy queue
	~CSphHitQueue ()
	{
		SafeDeleteArray ( m_pData );
	}

	/// add entry to the queue
	void Push ( CSphAggregateHit & tHit, int iBin )
	{
		// check for overflow and do add
		assert ( m_iUsed<m_iSize );
		m_pData [ m_iUsed ].m_iDocID = tHit.m_iDocID;
		m_pData [ m_iUsed ].m_iWordID = tHit.m_iWordID;
		m_pData [ m_iUsed ].m_sKeyword = tHit.m_sKeyword; // bin must hold the actual data for the queue
		m_pData [ m_iUsed ].m_iWordPos = tHit.m_iWordPos;
		m_pData [ m_iUsed ].m_dFieldMask = tHit.m_dFieldMask;
		m_pData [ m_iUsed ].m_iBin = iBin;

		int iEntry = m_iUsed++;

		// sift up if needed
		while ( iEntry )
		{
			int iParent = ( iEntry-1 ) >> 1;
			if ( SPH_CMPAGGRHIT_LESS ( m_pData[iEntry], m_pData[iParent] ) )
			{
				// entry is less than parent, should float to the top
				Swap ( m_pData[iEntry], m_pData[iParent] );
				iEntry = iParent;
			} else
			{
				break;
			}
		}
	}

	/// remove root (ie. top priority) entry
	void Pop ()
	{
		assert ( m_iUsed );
		if ( !(--m_iUsed) ) // empty queue? just return
			return;

		// make the last entry my new root
		m_pData[0] = m_pData[m_iUsed];

		// sift down if needed
		int iEntry = 0;
		for ( ;; )
		{
			// select child
			int iChild = (iEntry<<1) + 1;
			if ( iChild>=m_iUsed )
				break;

			// select smallest child
			if ( iChild+1<m_iUsed )
				if ( SPH_CMPAGGRHIT_LESS ( m_pData[iChild+1], m_pData[iChild] ) )
					iChild++;

			// if smallest child is less than entry, do float it to the top
			if ( SPH_CMPAGGRHIT_LESS ( m_pData[iChild], m_pData[iEntry] ) )
			{
				Swap ( m_pData[iChild], m_pData[iEntry] );
				iEntry = iChild;
				continue;
			}

			break;
		}
	}
};


struct CmpQueuedDocinfo_fn
{
	static DWORD *	m_pStorage;
	static int		m_iStride;

	static inline bool IsLess ( const int a, const int b )
	{
		return DOCINFO2ID ( m_pStorage + a*m_iStride ) < DOCINFO2ID ( m_pStorage + b*m_iStride );
	}
};
DWORD *		CmpQueuedDocinfo_fn::m_pStorage		= NULL;
int			CmpQueuedDocinfo_fn::m_iStride		= 1;


#define MAX_SOURCE_HITS	32768
static const int MIN_KEYWORDS_DICT	= 4*1048576;	// FIXME! ideally must be in sync with impl (ENTRY_CHUNKS, KEYWORD_CHUNKS)

/////////////////////////////////////////////////////////////////////////////

struct MvaEntry_t
{
	SphDocID_t	m_uDocID;
	int			m_iAttr;
	int64_t		m_iValue;

	inline bool operator < ( const MvaEntry_t & rhs ) const
	{
		if ( m_uDocID!=rhs.m_uDocID ) return m_uDocID<rhs.m_uDocID;
		if ( m_iAttr!=rhs.m_iAttr ) return m_iAttr<rhs.m_iAttr;
		return m_iValue<rhs.m_iValue;
	}
};


struct MvaEntryTag_t : public MvaEntry_t
{
	int			m_iTag;
};


struct MvaEntryCmp_fn
{
	static inline bool IsLess ( const MvaEntry_t & a, const MvaEntry_t & b )
	{
		return a<b;
	}
};


bool CSphIndex_VLN::BuildMVA ( const CSphVector<CSphSource*> & dSources, CSphFixedVector<CSphWordHit> & dHits,
		int iArenaSize, int iFieldFD, int nFieldMVAs, int iFieldMVAInPool, CSphIndex_VLN * pPrevIndex )
{
	// initialize writer (data file must always exist)
	CSphWriter wrMva;
	if ( !wrMva.OpenFile ( GetIndexFileName("spm"), m_sLastError ) )
		return false;

	// calcs and checks
	bool bOnlyFieldMVAs = true;
	CSphVector<int> dMvaIndexes;
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
		if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET )
		{
			dMvaIndexes.Add ( i );
			if ( tAttr.m_eSrc!=SPH_ATTRSRC_FIELD )
				bOnlyFieldMVAs = false;
		}
	}
	int iMva64 = dMvaIndexes.GetLength();
	// mva32 first
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
		if ( tAttr.m_eAttrType==SPH_ATTR_INT64SET )
		{
			dMvaIndexes.Add ( i );
			if ( tAttr.m_eSrc!=SPH_ATTRSRC_FIELD )
				bOnlyFieldMVAs = false;
		}
	}

	if ( dMvaIndexes.GetLength()<=0 )
		return true;

	// reuse hits pool
	MvaEntry_t * pMvaPool = (MvaEntry_t*) dHits.Begin();
	MvaEntry_t * pMvaMax = pMvaPool + ( iArenaSize/sizeof(MvaEntry_t) );
	MvaEntry_t * pMva = pMvaPool;

	// create temp file
	CSphAutofile fdTmpMva ( GetIndexFileName("tmp3"), SPH_O_NEW, m_sLastError, true );
	if ( fdTmpMva.GetFD()<0 )
		return false;

	//////////////////////////////
	// collect and partially sort
	//////////////////////////////

	CSphVector<int> dBlockLens;
	dBlockLens.Reserve ( 1024 );

	m_tProgress.m_ePhase = CSphIndexProgress::PHASE_COLLECT_MVA;

	if ( !bOnlyFieldMVAs )
	{
		ARRAY_FOREACH ( iSource, dSources )
		{
			CSphSource * pSource = dSources[iSource];
			if ( !pSource->Connect ( m_sLastError ) )
				return false;

			ARRAY_FOREACH ( i, dMvaIndexes )
			{
				int iAttr = dMvaIndexes[i];
				const CSphColumnInfo & tAttr = m_tSchema.GetAttr(iAttr);

				if ( tAttr.m_eSrc==SPH_ATTRSRC_FIELD )
					continue;

				if ( !pSource->IterateMultivaluedStart ( iAttr, m_sLastError ) )
					return false;

				while ( pSource->IterateMultivaluedNext () )
				{
					if ( pPrevIndex && pPrevIndex->FindDocinfo ( pSource->m_tDocInfo.m_iDocID ) )
						continue;

					pMva->m_uDocID = pSource->m_tDocInfo.m_iDocID;
					pMva->m_iAttr = i;
					if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET )
					{
						pMva->m_iValue = pSource->m_dMva[0];
					} else
					{
						pMva->m_iValue = MVA_UPSIZE ( pSource->m_dMva.Begin() );
					}

					if ( ++pMva>=pMvaMax )
					{
						sphSort ( pMvaPool, pMva-pMvaPool );
						if ( !sphWriteThrottled ( fdTmpMva.GetFD(), pMvaPool, (pMva-pMvaPool)*sizeof(MvaEntry_t), "temp_mva", m_sLastError, &g_tThrottle ) )
							return false;

						dBlockLens.Add ( pMva-pMvaPool );
						m_tProgress.m_iAttrs += pMva-pMvaPool;
						pMva = pMvaPool;

						m_tProgress.Show ( false );
					}
				}
			}

			pSource->Disconnect ();
		}

		if ( pMva>pMvaPool )
		{
			sphSort ( pMvaPool, pMva-pMvaPool );
			if ( !sphWriteThrottled ( fdTmpMva.GetFD(), pMvaPool, (pMva-pMvaPool)*sizeof(MvaEntry_t), "temp_mva", m_sLastError, &g_tThrottle ) )
				return false;

			dBlockLens.Add ( pMva-pMvaPool );
			m_tProgress.m_iAttrs += pMva-pMvaPool;
		}
	}

	m_tProgress.Show ( true );

	///////////////////////////
	// free memory for sorting
	///////////////////////////

	dHits.Reset ( 0 );

	//////////////
	// fully sort
	//////////////

	m_tProgress.m_ePhase = CSphIndexProgress::PHASE_SORT_MVA;
	m_tProgress.m_iAttrs = m_tProgress.m_iAttrs + nFieldMVAs;
	m_tProgress.m_iAttrsTotal = m_tProgress.m_iAttrs;
	m_tProgress.Show ( false );

	int	nLastBlockFieldMVAs = iFieldMVAInPool ? ( nFieldMVAs % iFieldMVAInPool ) : 0;
	int nFieldBlocks = iFieldMVAInPool ? ( nFieldMVAs / iFieldMVAInPool + ( nLastBlockFieldMVAs ? 1 : 0 ) ) : 0;

	// initialize readers
	CSphVector<CSphBin*> dBins;
	dBins.Reserve ( dBlockLens.GetLength() + nFieldBlocks );

	int iBinSize = CSphBin::CalcBinSize ( iArenaSize, dBlockLens.GetLength() + nFieldBlocks, "sort_mva" );
	SphOffset_t iSharedOffset = -1;

	ARRAY_FOREACH ( i, dBlockLens )
	{
		dBins.Add ( new CSphBin() );
		dBins[i]->m_iFileLeft = dBlockLens[i]*sizeof(MvaEntry_t);
		dBins[i]->m_iFilePos = ( i==0 ) ? 0 : dBins[i-1]->m_iFilePos + dBins[i-1]->m_iFileLeft;
		dBins[i]->Init ( fdTmpMva.GetFD(), &iSharedOffset, iBinSize );
	}

	SphOffset_t iSharedFieldOffset = -1;
	SphOffset_t uStart = 0;
	for ( int i = 0; i < nFieldBlocks; i++ )
	{
		dBins.Add ( new CSphBin() );
		int iBin = dBins.GetLength () - 1;

		dBins[iBin]->m_iFileLeft = sizeof(MvaEntry_t)*( i==nFieldBlocks-1
			? ( nLastBlockFieldMVAs ? nLastBlockFieldMVAs : iFieldMVAInPool )
			: iFieldMVAInPool );
		dBins[iBin]->m_iFilePos = uStart;
		dBins[iBin]->Init ( iFieldFD, &iSharedFieldOffset, iBinSize );

		uStart += dBins [iBin]->m_iFileLeft;
	}

	// do the sort
	CSphQueue < MvaEntryTag_t, MvaEntryCmp_fn > qMva ( Max ( 1, dBins.GetLength() ) );
	ARRAY_FOREACH ( i, dBins )
	{
		MvaEntryTag_t tEntry;
		if ( dBins[i]->ReadBytes ( (MvaEntry_t*) &tEntry, sizeof(MvaEntry_t) )!=BIN_READ_OK )
		{
			m_sLastError.SetSprintf ( "sort_mva: warmup failed (io error?)" );
			return false;
		}

		tEntry.m_iTag = i;
		qMva.Push ( tEntry );
	}

	// spm-file := info-list [ 0+ ]
	// info-list := docid, values-list [ index.schema.mva-count ]
	// values-list := values-count, value [ values-count ]
	// note that mva32 come first then mva64
	SphDocID_t uCurID = 0;
	CSphVector < CSphVector<int64_t> > dCurInfo;
	dCurInfo.Resize ( dMvaIndexes.GetLength() );

	for ( ;; )
	{
		// flush previous per-document info-list
		if ( !qMva.GetLength() || qMva.Root().m_uDocID!=uCurID )
		{
			if ( uCurID )
			{
				wrMva.PutDocid ( uCurID );
				ARRAY_FOREACH ( i, dCurInfo )
				{
					int iLen = dCurInfo[i].GetLength();
					if ( i>=iMva64 )
					{
						wrMva.PutDword ( iLen*2 );
						wrMva.PutBytes ( dCurInfo[i].Begin(), sizeof(int64_t)*iLen );
					} else
					{
						wrMva.PutDword ( iLen );
						ARRAY_FOREACH ( iVal, dCurInfo[i] )
						{
							wrMva.PutDword ( (DWORD)dCurInfo[i][iVal] );
						}
					}
				}
			}

			if ( !qMva.GetLength() )
				break;

			uCurID = qMva.Root().m_uDocID;
			ARRAY_FOREACH ( i, dCurInfo )
				dCurInfo[i].Resize ( 0 );
		}

		// accumulate this entry
#if PARANOID
		assert ( dCurInfo [ qMva.Root().m_iAttr ].GetLength()==0
			|| dCurInfo [ qMva.Root().m_iAttr ].Last()<=qMva.Root().m_iValue );
#endif
		dCurInfo [ qMva.Root().m_iAttr ].AddUnique ( qMva.Root().m_iValue );

		// get next entry
		int iBin = qMva.Root().m_iTag;
		qMva.Pop ();

		MvaEntryTag_t tEntry;
		ESphBinRead iRes = dBins[iBin]->ReadBytes ( (MvaEntry_t*)&tEntry, sizeof(MvaEntry_t) );
		tEntry.m_iTag = iBin;

		if ( iRes==BIN_READ_OK )
			qMva.Push ( tEntry );

		if ( iRes==BIN_READ_ERROR )
		{
			m_sLastError.SetSprintf ( "sort_mva: read error" );
			return false;
		}
	}

	// clean up readers
	ARRAY_FOREACH ( i, dBins )
		SafeDelete ( dBins[i] );

	wrMva.CloseFile ();
	if ( wrMva.IsError() )
		return false;

	m_tProgress.Show ( true );

	return true;
}


struct CmpOrdinalsValue_fn
{
	inline bool IsLess ( const Ordinal_t & a, const Ordinal_t & b ) const
	{
		return strcmp ( a.m_sValue.cstr(), b.m_sValue.cstr() )<0;
	}
};

struct CmpOrdinalsEntry_fn
{
	static inline bool IsLess ( const OrdinalEntry_t & a, const OrdinalEntry_t & b )
	{
		return strcmp ( a.m_sValue.cstr(), b.m_sValue.cstr() )<0;
	}
};

struct CmpOrdinalsDocid_fn
{
	inline bool IsLess ( const OrdinalId_t & a, const OrdinalId_t & b ) const
	{
		return a.m_uDocID < b.m_uDocID;
	}
};


struct CmpMvaEntries_fn
{
	inline bool IsLess ( const MvaEntry_t & a, const MvaEntry_t & b ) const
	{
		return a<b;
	}
};

struct CmpOrdinalIdEntry_fn
{
	static inline bool IsLess ( const OrdinalIdEntry_t & a, const OrdinalIdEntry_t & b )
	{
		return a.m_uDocID < b.m_uDocID;
	}
};


SphOffset_t CSphIndex_VLN::DumpOrdinals ( CSphWriter & Writer, CSphVector<Ordinal_t> & dOrdinals )
{
	SphOffset_t uSize = ( sizeof ( SphDocID_t ) + sizeof ( DWORD ) ) * dOrdinals.GetLength ();

	ARRAY_FOREACH ( i, dOrdinals )
	{
		Ordinal_t & Ord = dOrdinals[i];

		DWORD uValueLen = Ord.m_sValue.cstr () ? strlen ( Ord.m_sValue.cstr () ) : 0;
		Writer.PutBytes ( &(Ord.m_uDocID), sizeof ( Ord.m_uDocID ) );
		Writer.PutBytes ( &uValueLen, sizeof ( uValueLen ) );
		Writer.PutBytes ( Ord.m_sValue.cstr (), uValueLen );
		uSize += uValueLen;

		if ( Writer.IsError () )
			return 0;
	}

	return uSize;
}


ESphBinRead CSphIndex_VLN::ReadOrdinal ( CSphBin & Reader, Ordinal_t & Ordinal )
{
	ESphBinRead eRes = Reader.ReadBytes ( &Ordinal.m_uDocID, sizeof ( Ordinal.m_uDocID ) );
	if ( eRes!=BIN_READ_OK )
		return eRes;

	DWORD uStrLen;
	eRes = Reader.ReadBytes ( &uStrLen, sizeof ( DWORD ) );
	if ( eRes!=BIN_READ_OK )
		return eRes;

	if ( uStrLen>=(DWORD)MAX_ORDINAL_STR_LEN )
		return BIN_READ_ERROR;

	char dBuffer [MAX_ORDINAL_STR_LEN];

	if ( uStrLen > 0 )
	{
		eRes = Reader.ReadBytes ( dBuffer, uStrLen );
		if ( eRes!=BIN_READ_OK )
			return eRes;
	}

	dBuffer [uStrLen] = '\0';
	Ordinal.m_sValue = dBuffer;

	return BIN_READ_OK;
}


bool CSphIndex_VLN::SortOrdinals ( const char * szToFile, int iFromFD, int iArenaSize,
	int iOrdinalsInPool, CSphVector < CSphVector < SphOffset_t > > & dOrdBlockSize, bool bWarnOfMem )
{
	int nAttrs = dOrdBlockSize.GetLength ();
	int nBlocks = dOrdBlockSize[0].GetLength ();

	CSphWriter Writer;
	if ( !Writer.OpenFile ( szToFile, m_sLastError ) )
		return false;

	int iBinSize = CSphBin::CalcBinSize ( iArenaSize, nBlocks, "ordinals", bWarnOfMem );
	SphOffset_t iSharedOffset = -1;

	CSphQueue < OrdinalEntry_t, CmpOrdinalsEntry_fn > qOrdinals ( Max ( 1, nBlocks ) );
	OrdinalEntry_t tOrdinalEntry;
	DWORD uOrdinalId = 0;

	CSphVector < OrdinalId_t > dOrdinalIdPool;
	dOrdinalIdPool.Reserve ( nBlocks );

	CSphVector < CSphVector < SphOffset_t > > dStarts;
	dStarts.Resize ( nAttrs );
	ARRAY_FOREACH ( i, dStarts )
		dStarts[i].Resize ( nBlocks );

	SphOffset_t uStart = 0;
	for ( int iBlock = 0; iBlock < nBlocks; iBlock++ )
		for ( int iAttr = 0; iAttr < nAttrs; iAttr++ )
		{
			dStarts [iAttr][iBlock] = uStart;
			uStart += dOrdBlockSize [iAttr][iBlock];
		}

	for ( int iAttr = 0; iAttr < nAttrs; iAttr++ )
	{
		CSphVector < CSphBin > dBins;
		dBins.Resize ( nBlocks );

		ARRAY_FOREACH ( i, dBins )
		{
			dBins[i].m_iFileLeft = (int)dOrdBlockSize[iAttr][i];
			dBins[i].m_iFilePos = dStarts[iAttr][i];
			dBins[i].Init ( iFromFD, &iSharedOffset, iBinSize );
		}

		dOrdBlockSize [iAttr].Resize ( 0 );

		for ( int iBlock = 0; iBlock < nBlocks; iBlock++ )
		{
			if ( ReadOrdinal ( dBins [iBlock], tOrdinalEntry )!=BIN_READ_OK )
			{
				m_sLastError = "sort_ordinals: warmup failed (io error?)";
				return false;
			}

			tOrdinalEntry.m_iTag = iBlock;
			qOrdinals.Push ( tOrdinalEntry );
		}

		SphDocID_t uCurID = 0;

		CSphString sLastOrdValue;
		int iMyBlock = 0;

		for ( ;; )
		{
			if ( !qOrdinals.GetLength () || qOrdinals.Root ().m_uDocID!=uCurID )
			{
				if ( uCurID )
				{
					OrdinalId_t tId;
					tId.m_uDocID = uCurID;
					tId.m_uId = uOrdinalId;
					dOrdinalIdPool.Add ( tId );

					if ( qOrdinals.GetLength () > 0 )
					{
						if ( sLastOrdValue.cstr()[0]!=qOrdinals.Root ().m_sValue.cstr()[0] )
							uOrdinalId++;
						else
							if ( strcmp ( sLastOrdValue.cstr (), qOrdinals.Root ().m_sValue.cstr () ) )
								uOrdinalId++;
					}

					if ( dOrdinalIdPool.GetLength()==iOrdinalsInPool )
					{
						dOrdinalIdPool.Sort ( CmpOrdinalsDocid_fn () );
						Writer.PutBytes ( &dOrdinalIdPool[0], sizeof(OrdinalId_t)*dOrdinalIdPool.GetLength() );
						if ( Writer.IsError () )
						{
							m_sLastError = "sort_ordinals: io error";
							return false;
						}

						dOrdBlockSize [iAttr].Add ( dOrdinalIdPool.GetLength () * sizeof ( OrdinalId_t ) );
						dOrdinalIdPool.Resize ( 0 );
					}
				}

				if ( !qOrdinals.GetLength () )
					break;

				uCurID = qOrdinals.Root().m_uDocID;
				const_cast < CSphString & > ( qOrdinals.Root ().m_sValue ).Swap ( sLastOrdValue );
			}

			// get next entry
			iMyBlock = qOrdinals.Root().m_iTag;
			qOrdinals.Pop ();

			ESphBinRead eRes = ReadOrdinal ( dBins [iMyBlock], tOrdinalEntry );
			tOrdinalEntry.m_iTag = iMyBlock;
			if ( eRes==BIN_READ_OK )
				qOrdinals.Push ( tOrdinalEntry );

			if ( eRes==BIN_READ_ERROR )
			{
				m_sLastError = "sort_ordinals: read error";
				return false;
			}
		}

		// flush last ordinal ids
		if ( dOrdinalIdPool.GetLength () )
		{
			dOrdinalIdPool.Sort ( CmpOrdinalsDocid_fn () );
			Writer.PutBytes ( &dOrdinalIdPool[0], sizeof(OrdinalId_t)*dOrdinalIdPool.GetLength () );
			if ( Writer.IsError () )
			{
				m_sLastError = "sort_ordinals: io error";
				return false;
			}

			dOrdBlockSize [iAttr].Add ( dOrdinalIdPool.GetLength()*sizeof(OrdinalId_t) );
			dOrdinalIdPool.Resize ( 0 );
		}
	}

	Writer.CloseFile ();
	if ( Writer.IsError () )
		return false;

	return true;
}


bool CSphIndex_VLN::SortOrdinalIds ( const char * szToFile, int iFromFD, int iArenaSize,
	CSphVector < CSphVector < SphOffset_t > > & dOrdBlockSize, bool bWarnOfMem )
{
	int nAttrs = dOrdBlockSize.GetLength ();
	int nMaxBlocks = 0;
	ARRAY_FOREACH ( i, dOrdBlockSize )
		if ( dOrdBlockSize[i].GetLength () > nMaxBlocks )
			nMaxBlocks = dOrdBlockSize[i].GetLength ();

	CSphWriter Writer;
	if ( !Writer.OpenFile ( szToFile, m_sLastError ) )
		return false;

	int iBinSize = CSphBin::CalcBinSize ( iArenaSize, nMaxBlocks, "ordinals", bWarnOfMem );

	SphOffset_t uStart = 0;
	OrdinalIdEntry_t tOrdinalIdEntry;
	OrdinalId_t tOrdinalId;

	for ( int iAttr = 0; iAttr < nAttrs; ++iAttr )
	{
		int nBlocks = dOrdBlockSize [iAttr].GetLength ();
		CSphQueue < OrdinalIdEntry_t, CmpOrdinalIdEntry_fn > qOrdinalIds ( Max ( 1, nBlocks ) );
		CSphVector < CSphBin > dBins;
		dBins.Resize ( nBlocks );

		SphOffset_t iSharedOffset = -1;

		ARRAY_FOREACH ( i, dBins )
		{
			dBins[i].m_iFileLeft = (int)dOrdBlockSize [iAttr][i];
			dBins[i].m_iFilePos = uStart;
			dBins[i].Init ( iFromFD, &iSharedOffset, iBinSize );

			uStart += dBins[i].m_iFileLeft;
		}

		for ( int iBlock = 0; iBlock < nBlocks; iBlock++ )
		{
			if ( dBins[iBlock].ReadBytes ( &tOrdinalId, sizeof ( tOrdinalId ) )!=BIN_READ_OK )
			{
				m_sLastError = "sort_ordinals: warmup failed (io error?)";
				return false;
			}

			tOrdinalIdEntry.m_uDocID = tOrdinalId.m_uDocID;
			tOrdinalIdEntry.m_uId = tOrdinalId.m_uId;
			tOrdinalIdEntry.m_iTag = iBlock;
			qOrdinalIds.Push ( tOrdinalIdEntry );
		}

		OrdinalId_t tCachedId;
		tCachedId.m_uDocID = 0;

		SphOffset_t uResultSize = 0;

		for ( ;; )
		{
			if ( !qOrdinalIds.GetLength () || qOrdinalIds.Root ().m_uDocID!=tCachedId.m_uDocID )
			{
				if ( tCachedId.m_uDocID )
				{
					uResultSize += sizeof ( OrdinalId_t );
					Writer.PutBytes ( &tCachedId, sizeof ( OrdinalId_t ) );
					if ( Writer.IsError () )
					{
						m_sLastError = "sort_ordinals: io error";
						return false;
					}
				}

				if ( !qOrdinalIds.GetLength () )
					break;

				tCachedId.m_uDocID = qOrdinalIds.Root().m_uDocID;
				tCachedId.m_uId = qOrdinalIds.Root ().m_uId;
			}

			// get next entry
			int iBlock = qOrdinalIds.Root().m_iTag;
			qOrdinalIds.Pop ();

			ESphBinRead eRes = dBins [iBlock].ReadBytes ( &tOrdinalId, sizeof ( tOrdinalId ) );
			tOrdinalIdEntry.m_uDocID = tOrdinalId.m_uDocID;
			tOrdinalIdEntry.m_uId = tOrdinalId.m_uId;
			tOrdinalIdEntry.m_iTag = iBlock;
			if ( eRes==BIN_READ_OK )
				qOrdinalIds.Push ( tOrdinalIdEntry );

			if ( eRes==BIN_READ_ERROR )
			{
				m_sLastError = "sort_ordinals: read error";
				return false;
			}
		}

		dOrdBlockSize [iAttr].Resize ( 0 );
		dOrdBlockSize [iAttr].Add ( uResultSize );
	}

	return true;
}

struct FieldMVARedirect_t
{
	CSphAttrLocator		m_tLocator;
	int					m_iAttr;
	int					m_iMVAAttr;
	bool				m_bMva64;
};


bool CSphIndex_VLN::RelocateBlock ( int iFile, BYTE * pBuffer, int iRelocationSize,
	SphOffset_t * pFileSize, CSphBin * pMinBin, SphOffset_t * pSharedOffset )
{
	assert ( pBuffer && pFileSize && pMinBin && pSharedOffset );

	SphOffset_t iBlockStart = pMinBin->m_iFilePos;
	SphOffset_t iBlockLeft = pMinBin->m_iFileLeft;

	ESphBinRead eRes = pMinBin->Precache ();
	switch ( eRes )
	{
	case BIN_PRECACHE_OK:
		return true;
	case BIN_READ_ERROR:
		m_sLastError = "block relocation: preread error";
		return false;
	default:
		break;
	}

	int nTransfers = (int)( ( iBlockLeft+iRelocationSize-1) / iRelocationSize );

	SphOffset_t uTotalRead = 0;
	SphOffset_t uNewBlockStart = *pFileSize;

	for ( int i = 0; i < nTransfers; i++ )
	{
		sphSeek ( iFile, iBlockStart + uTotalRead, SEEK_SET );

		int iToRead = i==nTransfers-1 ? (int)( iBlockLeft % iRelocationSize ) : iRelocationSize;
		size_t iRead = sphReadThrottled ( iFile, pBuffer, iToRead, &g_tThrottle );
		if ( iRead!=size_t(iToRead) )
		{
			m_sLastError.SetSprintf ( "block relocation: read error (%d of %d bytes read): %s", (int)iRead, iToRead, strerror(errno) );
			return false;
		}

		sphSeek ( iFile, *pFileSize, SEEK_SET );
		uTotalRead += iToRead;

		if ( !sphWriteThrottled ( iFile, pBuffer, iToRead, "block relocation", m_sLastError, &g_tThrottle ) )
			return false;

		*pFileSize += iToRead;
	}

	assert ( uTotalRead==iBlockLeft );

	// update block pointers
	pMinBin->m_iFilePos = uNewBlockStart;
	*pSharedOffset = *pFileSize;

	return true;
}


static int CountWords ( const CSphString & sData, ISphTokenizer * pTokenizer )
{
	BYTE * sField = (BYTE*) sData.cstr();
	if ( !sField )
		return 0;

	int iCount = 0;
	pTokenizer->SetBuffer ( sField, (int)strlen ( (char*)sField ) );
	while ( pTokenizer->GetToken() )
		iCount++;
	return iCount;
}

bool CSphIndex_VLN::LoadHitlessWords ( CSphVector<SphWordID_t> & dHitlessWords )
{
	assert ( dHitlessWords.GetLength()==0 );

	if ( m_tSettings.m_sHitlessFiles.IsEmpty() )
		return true;

	const char * szStart = m_tSettings.m_sHitlessFiles.cstr();

	while ( *szStart )
	{
		while ( *szStart && ( sphIsSpace ( *szStart ) || *szStart==',' ) )
			++szStart;

		if ( !*szStart )
			break;

		const char * szWordStart = szStart;

		while ( *szStart && !sphIsSpace ( *szStart ) && *szStart!=',' )
			++szStart;

		if ( szStart - szWordStart > 0 )
		{
			CSphString sFilename;
			sFilename.SetBinary ( szWordStart, szStart-szWordStart );

			CSphAutofile tFile ( sFilename.cstr(), SPH_O_READ, m_sLastError );
			if ( tFile.GetFD()==-1 )
				return false;

			CSphVector<BYTE> dBuffer ( (int)tFile.GetSize() );
			if ( !tFile.Read ( &dBuffer[0], dBuffer.GetLength(), m_sLastError ) )
				return false;

			// FIXME!!! dict=keywords + hitless_words=some
			m_pTokenizer->SetBuffer ( &dBuffer[0], dBuffer.GetLength() );
			while ( BYTE * sToken = m_pTokenizer->GetToken() )
				dHitlessWords.Add ( m_pDict->GetWordID ( sToken ) );
		}
	}

	dHitlessWords.Uniq();
	return true;
}


static bool sphTruncate ( int iFD )
{
#if USE_WINDOWS
	return SetEndOfFile ( (HANDLE) _get_osfhandle(iFD) )!=0;
#else
	return ::ftruncate ( iFD, ::lseek ( iFD, 0, SEEK_CUR ) )==0;
#endif
}

class DeleteOnFail : public ISphNoncopyable
{
public:
	DeleteOnFail() : m_bShitHappened ( true )
	{}
	inline ~DeleteOnFail()
	{
		if ( m_bShitHappened )
		{
			ARRAY_FOREACH ( i, m_dWriters )
				m_dWriters[i]->UnlinkFile();

			ARRAY_FOREACH ( i, m_dAutofiles )
				m_dAutofiles[i]->SetTemporary();
		}
	}
	inline void AddWriter ( CSphWriter* pWr )
	{
		if ( pWr )
			m_dWriters.Add ( pWr );
	}
	inline void AddAutofile ( CSphAutofile* pAf )
	{
		if ( pAf )
			m_dAutofiles.Add ( pAf );
	}
	inline void AllIsDone()
	{
		m_bShitHappened = false;
	}
private:
	bool	m_bShitHappened;
	CSphVector<CSphWriter*> m_dWriters;
	CSphVector<CSphAutofile*> m_dAutofiles;
};


int CSphIndex_VLN::Build ( const CSphVector<CSphSource*> & dSources, int iMemoryLimit, int iWriteBuffer )
{
	assert ( dSources.GetLength() );

	CSphVector<SphWordID_t> dHitlessWords;

	if ( !LoadHitlessWords ( dHitlessWords ) )
		return 0;

	int iHitBuilderBufferSize = ( iWriteBuffer>0 )
		? Max ( iWriteBuffer, MIN_WRITE_BUFFER )
		: DEFAULT_WRITE_BUFFER;

	// vars shared between phases
	CSphVector<CSphBin*> dBins;
	SphOffset_t iSharedOffset = -1;

	m_pDict->HitblockBegin();

	// setup sources
	ARRAY_FOREACH ( iSource, dSources )
	{
		CSphSource * pSource = dSources[iSource];
		assert ( pSource );

		pSource->SetDict ( m_pDict );
		pSource->Setup ( m_tSettings );
	}

	// connect 1st source and fetch its schema
	if ( !dSources[0]->Connect ( m_sLastError )
		|| !dSources[0]->IterateStart ( m_sLastError )
		|| !dSources[0]->UpdateSchema ( &m_tSchema, m_sLastError ) )
	{
		return 0;
	}

	if ( m_tSchema.m_dFields.GetLength()==0 )
	{
		m_sLastError.SetSprintf ( "No fields in schema - will not index" );
		return 0;
	}

	// check docinfo
	if ( m_tSchema.GetAttrsCount()==0 && m_tSettings.m_eDocinfo!=SPH_DOCINFO_NONE )
	{
		sphWarning ( "Attribute count is 0: switching to none docinfo" );
		m_tSettings.m_eDocinfo = SPH_DOCINFO_NONE;
	}

	if ( dSources[0]->HasJoinedFields() && m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE )
	{
		m_sLastError.SetSprintf ( "got joined fields, but docinfo is 'inline' (fix your config file)" );
		return 0;
	}

	if ( m_tSchema.GetAttrsCount()>0 && m_tSettings.m_eDocinfo==SPH_DOCINFO_NONE )
	{
		m_sLastError.SetSprintf ( "got attributes, but docinfo is 'none' (fix your config file)" );
		return 0;
	}

	bool bHaveFieldMVAs = false;
	int iFieldLens = -1;
	CSphVector<int> dMvaIndexes;
	CSphVector<CSphAttrLocator> dMvaLocators;

	// ordinals and strings storage
	CSphVector<int> dOrdinalAttrs;
	CSphVector<int> dStringAttrs;
	CSphVector<int> dWordcountAttrs;

	// chunks to partically sort string attributes
	CSphVector<DWORD> dStringChunks;
	SphOffset_t uStringChunk = 0;

	// Sphinx-BSON storage
	CSphVector<BYTE> dBson;
	dBson.Reserve ( 1024 );

	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tCol = m_tSchema.GetAttr(i);
		switch ( tCol.m_eAttrType )
		{
			case SPH_ATTR_UINT32SET:
				if ( tCol.m_eSrc==SPH_ATTRSRC_FIELD )
					bHaveFieldMVAs = true;
				dMvaIndexes.Add ( i );
				dMvaLocators.Add ( tCol.m_tLocator );
				break;
			case SPH_ATTR_ORDINAL:
				if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN )
					dOrdinalAttrs.Add ( i );
				break;
			case SPH_ATTR_STRING:
			case SPH_ATTR_JSON:
				dStringAttrs.Add ( i );
				break;
			case SPH_ATTR_WORDCOUNT:
				dWordcountAttrs.Add ( i );
				break;
			case SPH_ATTR_TOKENCOUNT:
				if ( iFieldLens<0 )
					iFieldLens = i;
				break;
			default:
				break;
		}
	}

	// no field lengths for docinfo=inline
	assert ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN || iFieldLens==-1 );

	// this loop must NOT be merged with the previous one;
	// mva64 must intentionally be after all the mva32
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tCol = m_tSchema.GetAttr(i);
		if ( tCol.m_eAttrType!=SPH_ATTR_INT64SET )
			continue;
		if ( tCol.m_eSrc==SPH_ATTRSRC_FIELD )
			bHaveFieldMVAs = true;
		dMvaIndexes.Add ( i );
		dMvaLocators.Add ( tCol.m_tLocator );
	}

	bool bGotMVA = ( dMvaIndexes.GetLength()!=0 );
	if ( bGotMVA && m_tSettings.m_eDocinfo!=SPH_DOCINFO_EXTERN )
	{
		m_sLastError.SetSprintf ( "multi-valued attributes require docinfo=extern (fix your config file)" );
		return 0;
	}

	bool bHaveOrdinals = ( dOrdinalAttrs.GetLength() > 0 );
	if ( bHaveOrdinals && m_tSettings.m_eDocinfo!=SPH_DOCINFO_EXTERN )
	{
		m_sLastError.SetSprintf ( "ordinal string attributes require docinfo=extern (fix your config file)" );
		return 0;
	}

	if ( dStringAttrs.GetLength() && m_tSettings.m_eDocinfo!=SPH_DOCINFO_EXTERN )
	{
		m_sLastError.SetSprintf ( "string attributes require docinfo=extern (fix your config file)" );
		return 0;
	}

	CSphHitBuilder tHitBuilder ( m_tSettings, dHitlessWords, false, iHitBuilderBufferSize, m_pDict, &m_sLastError );

	////////////////////////////////////////////////
	// collect and partially sort hits and docinfos
	////////////////////////////////////////////////

	// killlist storage
	CSphVector <SphAttr_t> dKillList;

	// adjust memory requirements
	int iOldLimit = iMemoryLimit;

	// book memory to store at least 64K attribute rows
	const int iDocinfoStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
	int iDocinfoMax = Max ( iMemoryLimit/16/iDocinfoStride/sizeof(DWORD), 65536ul );
	if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_NONE )
		iDocinfoMax = 1;

	// book at least 32 KB for ordinals, if needed
	int iOrdinalPoolSize = Max ( 32768, iMemoryLimit/8 );
	if ( !bHaveOrdinals )
		iOrdinalPoolSize = 0;

	// book at least 32 KB for field MVAs, if needed
	int iFieldMVAPoolSize = Max ( 32768, iMemoryLimit/16 );
	if ( bHaveFieldMVAs==0 )
		iFieldMVAPoolSize = 0;

	// book at least 2 MB for keywords dict, if needed
	int iDictSize = 0;
	if ( m_pDict->GetSettings().m_bWordDict )
		iDictSize = Max ( MIN_KEYWORDS_DICT, iMemoryLimit/8 );

	// do we have enough left for hits?
	int iHitsMax = 1048576;

	iMemoryLimit -= iDocinfoMax*iDocinfoStride*sizeof(DWORD) + iOrdinalPoolSize + iFieldMVAPoolSize + iDictSize;
	if ( iMemoryLimit < iHitsMax*(int)sizeof(CSphWordHit) )
	{
		iMemoryLimit = iOldLimit + iHitsMax*sizeof(CSphWordHit) - iMemoryLimit;
		sphWarn ( "collect_hits: mem_limit=%d kb too low, increasing to %d kb",
			iOldLimit/1024, iMemoryLimit/1024 );
	} else
	{
		iHitsMax = iMemoryLimit / sizeof(CSphWordHit);
	}

	// allocate raw hits block
	CSphFixedVector<CSphWordHit> dHits ( iHitsMax + MAX_SOURCE_HITS );
	CSphWordHit * pHits = dHits.Begin();
	CSphWordHit * pHitsMax = dHits.Begin() + iHitsMax;

	// after finishing with hits this pool will be used to sort strings
	int iPoolSize = dHits.GetSizeBytes();

	// allocate docinfos buffer
	CSphFixedVector<DWORD> dDocinfos ( iDocinfoMax*iDocinfoStride );
	DWORD * pDocinfo = dDocinfos.Begin();
	const DWORD * pDocinfoMax = dDocinfos.Begin() + iDocinfoMax*iDocinfoStride;
	if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_NONE )
	{
		pDocinfo = NULL;
		pDocinfoMax = NULL;
	}

	int nOrdinals = 0;
	SphOffset_t uMaxOrdinalAttrBlockSize = 0;
	int iCurrentBlockSize = 0;

	CSphVector < CSphVector < Ordinal_t > > dOrdinals;
	dOrdinals.Resize ( dOrdinalAttrs.GetLength() );
	ARRAY_FOREACH ( i, dOrdinals )
		dOrdinals[i].Reserve ( 65536 );

	CSphVector < CSphVector<SphOffset_t> > dOrdBlockSize;
	dOrdBlockSize.Resize ( dOrdinalAttrs.GetLength () );
	ARRAY_FOREACH ( i, dOrdBlockSize )
		dOrdBlockSize[i].Reserve ( 8192 );

	int iMaxOrdLen = 0;

	CSphVector < MvaEntry_t > dFieldMVAs;
	dFieldMVAs.Reserve ( 16384 );

	CSphVector < SphOffset_t > dFieldMVABlocks;
	dFieldMVABlocks.Reserve ( 4096 );

	CSphVector < FieldMVARedirect_t > dFieldMvaIndexes;

	if ( bHaveFieldMVAs )
		dFieldMvaIndexes.Reserve ( 8 );

	int iMaxPoolFieldMVAs = iFieldMVAPoolSize / sizeof ( MvaEntry_t );
	int nFieldMVAs = 0;

	CSphScopedPtr<CSphIndex_VLN> pPrevIndex(NULL);
	if ( m_bKeepAttrs )
	{
		CSphString sWarning;
		pPrevIndex = (CSphIndex_VLN *)sphCreateIndexPhrase ( NULL, m_sFilename.cstr() );
		pPrevIndex->SetWordlistPreload ( false );
		if ( !pPrevIndex->Prealloc ( false, false, sWarning ) || !pPrevIndex->Preread() )
			pPrevIndex.Reset();
		else
		{
			// check schemas
			CSphString sError;
			if ( !m_tSchema.CompareTo ( pPrevIndex->m_tSchema, sError, false ) )
			{
				sphWarn ( "schemas are different (%s); ignoring --keep-attrs", sError.cstr() );
				pPrevIndex.Reset();
			}
		}
	}

	// create temp files
	CSphAutofile fdLock ( GetIndexFileName("tmp0"), SPH_O_NEW, m_sLastError, true );
	CSphAutofile fdHits ( GetIndexFileName ( m_bInplaceSettings ? "spp" : "tmp1" ), SPH_O_NEW, m_sLastError, !m_bInplaceSettings );
	CSphAutofile fdDocinfos ( GetIndexFileName ( m_bInplaceSettings ? "spa" : "tmp2" ), SPH_O_NEW, m_sLastError, !m_bInplaceSettings );
	CSphAutofile fdTmpFieldMVAs ( GetIndexFileName("tmp7"), SPH_O_NEW, m_sLastError, true );
	CSphWriter tOrdWriter;
	CSphWriter tStrWriter;
	CSphWriter tStrFinalWriter;

	CSphString sRawOrdinalsFile = GetIndexFileName("tmp4");
	if ( bHaveOrdinals && !tOrdWriter.OpenFile ( sRawOrdinalsFile.cstr (), m_sLastError ) )
		return 0;

	if ( !tStrWriter.OpenFile ( GetIndexFileName("tmps"), m_sLastError ) )
		return 0;
	tStrWriter.PutByte ( 0 ); // dummy byte, to reserve magic zero offset

	if ( !tStrFinalWriter.OpenFile ( GetIndexFileName("sps"), m_sLastError ) )
		return 0;
	tStrFinalWriter.PutByte ( 0 ); // dummy byte, to reserve magic zero offset

	DeleteOnFail dFileWatchdog;

	if ( m_bInplaceSettings )
	{
		dFileWatchdog.AddAutofile ( &fdHits );
		dFileWatchdog.AddAutofile ( &fdDocinfos );
	}

	dFileWatchdog.AddWriter ( &tStrWriter );
	dFileWatchdog.AddWriter ( &tStrFinalWriter );

	if ( fdLock.GetFD()<0 || fdHits.GetFD()<0 || fdDocinfos.GetFD()<0 || fdTmpFieldMVAs.GetFD ()<0 )
		return 0;

	SphOffset_t iHitsGap = 0;
	SphOffset_t iDocinfosGap = 0;

	if ( m_bInplaceSettings )
	{
		const int HIT_SIZE_AVG = 4;
		const float HIT_BLOCK_FACTOR = 1.0f;
		const float DOCINFO_BLOCK_FACTOR = 1.0f;

		if ( m_iHitGap )
			iHitsGap = (SphOffset_t) m_iHitGap;
		else
			iHitsGap = (SphOffset_t)( iHitsMax*HIT_BLOCK_FACTOR*HIT_SIZE_AVG );

		iHitsGap = Max ( iHitsGap, 1 );
		sphSeek ( fdHits.GetFD (), iHitsGap, SEEK_SET );

		if ( m_iDocinfoGap )
			iDocinfosGap = (SphOffset_t) m_iDocinfoGap;
		else
			iDocinfosGap = (SphOffset_t)( iDocinfoMax*DOCINFO_BLOCK_FACTOR*iDocinfoStride*sizeof(DWORD) );

		iDocinfosGap = Max ( iDocinfosGap, 1 );
		sphSeek ( fdDocinfos.GetFD (), iDocinfosGap, SEEK_SET );
	}

	if ( !sphLockEx ( fdLock.GetFD(), false ) )
	{
		m_sLastError.SetSprintf ( "failed to lock '%s': another indexer running?", fdLock.GetFilename() );
		return 0;
	}

	// setup accumulating docinfo IDs range
	m_dMinRow.Reset ( m_tSchema.GetRowSize() );
	m_iMinDocid = DOCID_MAX;
	ARRAY_FOREACH ( i, m_dMinRow )
		m_dMinRow[i] = ROWITEM_MAX;

	m_tStats.Reset ();
	m_tProgress.m_ePhase = CSphIndexProgress::PHASE_COLLECT;
	m_tProgress.m_iAttrs = 0;

	CSphVector<int> dHitBlocks;
	dHitBlocks.Reserve ( 1024 );

	int iDocinfoBlocks = 0;

	ARRAY_FOREACH ( iSource, dSources )
	{
		// connect and check schema, if it's not the first one
		CSphSource * pSource = dSources[iSource];

		if ( iSource )
		{
			if ( !pSource->Connect ( m_sLastError )
				|| !pSource->IterateStart ( m_sLastError )
				|| !pSource->UpdateSchema ( &m_tSchema, m_sLastError ) )
			{
				return 0;
			}

			if ( pSource->HasJoinedFields() && m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE )
			{
				m_sLastError.SetSprintf ( "got joined fields, but docinfo is 'inline' (fix your config file)" );
				return 0;
			}
		}

		dFieldMvaIndexes.Resize ( 0 );

		ARRAY_FOREACH ( i, dMvaIndexes )
		{
			int iAttr = dMvaIndexes[i];
			const CSphColumnInfo & tCol = m_tSchema.GetAttr ( iAttr );
			if ( tCol.m_eSrc==SPH_ATTRSRC_FIELD )
			{
				FieldMVARedirect_t & tRedirect = dFieldMvaIndexes.Add();
				tRedirect.m_tLocator = tCol.m_tLocator;
				tRedirect.m_iAttr = iAttr;
				tRedirect.m_iMVAAttr = i;
				tRedirect.m_bMva64 = ( tCol.m_eAttrType==SPH_ATTR_INT64SET );
			}
		}

		// joined filter
		bool bGotJoined = ( m_tSettings.m_eDocinfo!=SPH_DOCINFO_INLINE ) && pSource->HasJoinedFields();

		// fetch documents
		for ( ;; )
		{
			// get next doc, and handle errors
			bool bGotDoc = pSource->IterateDocument ( m_sLastError );
			if ( !bGotDoc )
				return 0;

			// ensure docid is sane
			if ( pSource->m_tDocInfo.m_iDocID==DOCID_MAX )
			{
				m_sLastError.SetSprintf ( "docid==DOCID_MAX (source broken?)" );
				return 0;
			}

			// check for eof
			if ( !pSource->m_tDocInfo.m_iDocID )
				break;

			// show progress bar
			if ( ( pSource->GetStats().m_iTotalDocuments % 1000 )==0 )
			{
				m_tProgress.m_iDocuments = m_tStats.m_iTotalDocuments + pSource->GetStats().m_iTotalDocuments;
				m_tProgress.m_iBytes = m_tStats.m_iTotalBytes + pSource->GetStats().m_iTotalBytes;
				m_tProgress.Show ( false );
			}

			// update crashdump
			g_iIndexerCurrentDocID = pSource->m_tDocInfo.m_iDocID;
			g_iIndexerCurrentHits = pHits-dHits.Begin();

			DWORD * pPrevDocinfo = NULL;
			if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN && pPrevIndex.Ptr() )
				pPrevDocinfo = const_cast<DWORD*>( pPrevIndex->FindDocinfo ( pSource->m_tDocInfo.m_iDocID ) );

			if ( dMvaIndexes.GetLength() && pPrevDocinfo && pPrevIndex->m_tMva.GetWritePtr() )
			{
				// fetch old mva values
				ARRAY_FOREACH ( i, dMvaIndexes )
				{
					const CSphColumnInfo & tCol = m_tSchema.GetAttr ( dMvaIndexes[i] );
					SphAttr_t uOff = sphGetRowAttr ( DOCINFO2ATTRS ( pPrevDocinfo ), tCol.m_tLocator );
					if ( !uOff )
						continue;

					const DWORD * pMVA = pPrevIndex->m_tMva.GetWritePtr()+uOff;
					int nMVAs = *pMVA++;
					for ( int iMVA = 0; iMVA < nMVAs; iMVA++ )
					{
						MvaEntry_t & tMva = dFieldMVAs.Add();
						tMva.m_uDocID = pSource->m_tDocInfo.m_iDocID;
						tMva.m_iAttr = i;
						if ( tCol.m_eAttrType==SPH_ATTR_INT64SET )
						{
							tMva.m_iValue = MVA_UPSIZE(pMVA);
							pMVA++;
						} else
							tMva.m_iValue = *pMVA;

						pMVA++;

						int iLength = dFieldMVAs.GetLength ();
						if ( iLength==iMaxPoolFieldMVAs )
						{
							dFieldMVAs.Sort ( CmpMvaEntries_fn () );
							if ( !sphWriteThrottled ( fdTmpFieldMVAs.GetFD (), &dFieldMVAs[0],
								iLength*sizeof(MvaEntry_t), "temp_field_mva", m_sLastError, &g_tThrottle ) )
								return 0;

							dFieldMVAs.Resize ( 0 );

							nFieldMVAs += iMaxPoolFieldMVAs;
						}
					}
				}

			} else if ( bHaveFieldMVAs )
			{
				// store field MVAs
				ARRAY_FOREACH ( i, dFieldMvaIndexes )
				{
					int iAttr = dFieldMvaIndexes[i].m_iAttr;
					int iMVA = dFieldMvaIndexes[i].m_iMVAAttr;
					bool bMva64 = dFieldMvaIndexes[i].m_bMva64;
					int iStep = ( bMva64 ? 2 : 1 );

					// store per-document MVAs
					SphRange_t tFieldMva = pSource->IterateFieldMVAStart ( iAttr );
					m_tProgress.m_iAttrs += ( tFieldMva.m_iLength / iStep );

					assert ( ( tFieldMva.m_iStart + tFieldMva.m_iLength )<=pSource->m_dMva.GetLength() );
					for ( int j=tFieldMva.m_iStart; j<( tFieldMva.m_iStart+tFieldMva.m_iLength); j+=iStep )
					{
						MvaEntry_t & tMva = dFieldMVAs.Add();
						tMva.m_uDocID = pSource->m_tDocInfo.m_iDocID;
						tMva.m_iAttr = iMVA;
						if ( bMva64 )
						{
							tMva.m_iValue = MVA_UPSIZE ( pSource->m_dMva.Begin() + j );
						} else
						{
							tMva.m_iValue = pSource->m_dMva[j];
						}

						int iLength = dFieldMVAs.GetLength ();
						if ( iLength==iMaxPoolFieldMVAs )
						{
							dFieldMVAs.Sort ( CmpMvaEntries_fn () );
							if ( !sphWriteThrottled ( fdTmpFieldMVAs.GetFD (), &dFieldMVAs[0],
								iLength*sizeof(MvaEntry_t), "temp_field_mva", m_sLastError, &g_tThrottle ) )
									return 0;

							dFieldMVAs.Resize ( 0 );

							nFieldMVAs += iMaxPoolFieldMVAs;
						}
					}
				}
			}

			// store ordinals
			iCurrentBlockSize += ( sizeof ( SphOffset_t ) + sizeof ( DWORD ) ) * dOrdinalAttrs.GetLength ();

			ARRAY_FOREACH ( i, dOrdinalAttrs )
			{
				CSphVector<Ordinal_t> & dCol = dOrdinals[i];
				dCol.Add();

				Ordinal_t & tLastOrd = dCol.Last();
				tLastOrd.m_uDocID = pSource->m_tDocInfo.m_iDocID;
				Swap ( tLastOrd.m_sValue, pSource->m_dStrAttrs[dOrdinalAttrs[i]] );
				int iOrdStrLen = strlen ( tLastOrd.m_sValue.cstr () );
				if ( iOrdStrLen > MAX_ORDINAL_STR_LEN )
				{
					iMaxOrdLen = iOrdStrLen;

					// truncate
					iOrdStrLen = MAX_ORDINAL_STR_LEN;
					tLastOrd.m_sValue = tLastOrd.m_sValue.SubString ( 0, iOrdStrLen - 1 );
				}

				iCurrentBlockSize += iOrdStrLen;
			}

			if ( bHaveOrdinals )
			{
				if ( iCurrentBlockSize>=iOrdinalPoolSize )
				{
					iCurrentBlockSize = 0;

					nOrdinals += dOrdinals[0].GetLength ();

					ARRAY_FOREACH ( i, dOrdinalAttrs )
					{
						CSphVector<Ordinal_t> & dCol = dOrdinals[i];
						dCol.Sort ( CmpOrdinalsValue_fn() );
						SphOffset_t uSize = DumpOrdinals ( tOrdWriter, dCol );
						if ( !uSize )
						{
							m_sLastError = "dump ordinals: io error";
							return 0;
						}

						if ( uSize > uMaxOrdinalAttrBlockSize )
							uMaxOrdinalAttrBlockSize = uSize;

						dOrdBlockSize[i].Add ( uSize );
						dCol.Resize ( 0 );
					}
				}
			}

			// store strings and JSON blobs
			if ( pPrevDocinfo )
			{
				CSphRowitem * pPrevAttrs = DOCINFO2ATTRS ( pPrevDocinfo );
				ARRAY_FOREACH ( i, dStringAttrs )
				{
					const CSphAttrLocator & tLoc = m_tSchema.GetAttr ( dStringAttrs[i] ).m_tLocator;
					SphAttr_t uPrevOff = sphGetRowAttr ( pPrevAttrs, tLoc );
					BYTE * pBase = pPrevIndex->m_tString.GetWritePtr();
					if ( !uPrevOff || !pBase )
						sphSetRowAttr ( pPrevAttrs, tLoc, 0 );
					else
					{
						const BYTE * pStr = NULL;
						int iLen = sphUnpackStr ( pBase+uPrevOff, &pStr );
						if ( !iLen )
							sphSetRowAttr ( pPrevAttrs, tLoc, 0 );
						else
						{
							SphOffset_t uOff = tStrWriter.GetPos();
							if ( uint64_t(uOff)>>32 )
							{
								m_sLastError.SetSprintf ( "too many string attributes (current index format allows up to 4 GB)" );
								return 0;
							}

							sphSetRowAttr ( pPrevAttrs, tLoc, DWORD(uOff) );

							BYTE dPackedLen[4];
							int iLenLen = sphPackStrlen ( dPackedLen, iLen );
							tStrWriter.PutBytes ( &dPackedLen, iLenLen );
							tStrWriter.PutBytes ( pStr, iLen );
						}
					}
				}
			} else
			{
				ARRAY_FOREACH ( i, dStringAttrs )
				{
					// FIXME! optimize locators etc?
					// FIXME! support binary strings w/embedded zeroes?
					// get data, calc length
					const char * sData = pSource->m_dStrAttrs[dStringAttrs[i]].cstr();
					int iLen = sData ? strlen ( sData ) : 0;

					// no data
					if ( !iLen )
					{
						pSource->m_tDocInfo.SetAttr ( m_tSchema.GetAttr ( dStringAttrs[i] ).m_tLocator, 0 );
						continue;
					}

					// handle JSON
					if ( m_tSchema.GetAttr ( dStringAttrs[i] ).m_eAttrType==SPH_ATTR_JSON ) // FIXME? optimize?
					{
						// WARNING, tricky bit
						// flex lexer needs last two (!) bytes to be zeroes
						// asciiz string supplies one, and we fill out the extra one
						// and that works, because CSphString always allocates a small extra gap
						char * pData = const_cast<char*>(sData);
						pData[iLen+1] = '\0';

						dBson.Resize ( 0 );
						if ( !sphJsonParse ( dBson, pData, g_bJsonAutoconvNumbers, g_bJsonKeynamesToLowercase, m_sLastError ) )
						{
							m_sLastError.SetSprintf ( "document " DOCID_FMT ", attribute %s: JSON error: %s",
								pSource->m_tDocInfo.m_iDocID, m_tSchema.GetAttr ( dStringAttrs[i] ).m_sName.cstr(),
								m_sLastError.cstr() );

							// bail?
							if ( g_bJsonStrict )
								return 0;

							// warn and ignore
							sphWarning ( "%s", m_sLastError.cstr() );
							m_sLastError = "";
							pSource->m_tDocInfo.SetAttr ( m_tSchema.GetAttr ( dStringAttrs[i] ).m_tLocator, 0 );
							continue;
						}
						if ( !dBson.GetLength() )
						{
							// empty SphinxBSON, need not save any data
							pSource->m_tDocInfo.SetAttr ( m_tSchema.GetAttr ( dStringAttrs[i] ).m_tLocator, 0 );
							continue;
						}

						// let's go save the newly built SphinxBSON blob
						sData = (const char*)dBson.Begin();
						iLen = dBson.GetLength();
					}

					// calc offset, do sanity checks
					SphOffset_t uOff = tStrWriter.GetPos();
					if ( uint64_t(uOff)>>32 )
					{
						m_sLastError.SetSprintf ( "too many string attributes (current index format allows up to 4 GB)" );
						return 0;
					}
					pSource->m_tDocInfo.SetAttr ( m_tSchema.GetAttr ( dStringAttrs[i] ).m_tLocator, DWORD(uOff) );

					// pack length, emit it, emit data
					BYTE dPackedLen[4];
					int iLenLen = sphPackStrlen ( dPackedLen, iLen );
					tStrWriter.PutBytes ( &dPackedLen, iLenLen );
					tStrWriter.PutBytes ( sData, iLen );

					// check if current pos is the good one for sorting
					if ( uOff+iLenLen+iLen-uStringChunk > iPoolSize )
					{
						dStringChunks.Add ( DWORD ( uOff-uStringChunk ) );
						uStringChunk = uOff;
					}
				}
			}

			// count words
			if ( !pPrevDocinfo )
				ARRAY_FOREACH ( i, dWordcountAttrs )
				{
					int iAttr = dWordcountAttrs[i];
					int iNumWords = CountWords ( pSource->m_dStrAttrs[iAttr], m_pTokenizer );
					pSource->m_tDocInfo.SetAttr ( m_tSchema.GetAttr(iAttr).m_tLocator, iNumWords );
				}

			// docinfo=inline might be flushed while collecting hits
			if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE )
			{
				// store next entry
				DOCINFOSETID ( pDocinfo, pSource->m_tDocInfo.m_iDocID );
				memcpy ( DOCINFO2ATTRS ( pDocinfo ), pSource->m_tDocInfo.m_pDynamic, sizeof(CSphRowitem)*m_tSchema.GetRowSize() );
				pDocinfo += iDocinfoStride;

				// update min docinfo
				assert ( pSource->m_tDocInfo.m_iDocID );
				m_iMinDocid = Min ( m_iMinDocid, pSource->m_tDocInfo.m_iDocID );
				ARRAY_FOREACH ( i, m_dMinRow )
					m_dMinRow[i] = Min ( m_dMinRow[i], pSource->m_tDocInfo.m_pDynamic[i] );
			}

			// store hits
			while ( const ISphHits * pDocHits = pSource->IterateHits ( m_sLastWarning ) )
			{
				int iDocHits = pDocHits->Length();
#if PARANOID
				for ( int i=0; i<iDocHits; i++ )
				{
					assert ( pDocHits->m_dData[i].m_iDocID==pSource->m_tDocInfo.m_iDocID );
					assert ( pDocHits->m_dData[i].m_iWordID );
					assert ( pDocHits->m_dData[i].m_iWordPos );
				}
#endif

				assert ( ( pHits+iDocHits )<=( pHitsMax+MAX_SOURCE_HITS ) );

				memcpy ( pHits, pDocHits->First(), iDocHits*sizeof(CSphWordHit) );
				pHits += iDocHits;

				// check if we need to flush
				if ( pHits<pHitsMax
					&& !( m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE && pDocinfo>=pDocinfoMax )
					&& !( iDictSize && m_pDict->HitblockGetMemUse() > iDictSize ) )
				{
					continue;
				}

				// update crashdump
				g_iIndexerPoolStartDocID = pSource->m_tDocInfo.m_iDocID;
				g_iIndexerPoolStartHit = pHits-dHits.Begin();

				// sort hits
				int iHits = pHits - dHits.Begin();
				{
					sphSort ( dHits.Begin(), iHits, CmpHit_fn() );
					m_pDict->HitblockPatch ( dHits.Begin(), iHits );
				}
				pHits = dHits.Begin();

				if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE )
				{
					// we're inlining, so let's flush both hits and docs
					int iDocs = ( pDocinfo - dDocinfos.Begin() ) / iDocinfoStride;
					pDocinfo = dDocinfos.Begin();

					sphSortDocinfos ( dDocinfos.Begin(), iDocs, iDocinfoStride );

					dHitBlocks.Add ( tHitBuilder.cidxWriteRawVLB ( fdHits.GetFD(), dHits.Begin(), iHits,
						dDocinfos.Begin(), iDocs, iDocinfoStride ) );

					// we are inlining, so if there are more hits in this document,
					// we'll need to know it's info next flush
					if ( iDocHits )
					{
						DOCINFOSETID ( pDocinfo, pSource->m_tDocInfo.m_iDocID );
						memcpy ( DOCINFO2ATTRS ( pDocinfo ), pSource->m_tDocInfo.m_pDynamic, sizeof(CSphRowitem)*m_tSchema.GetRowSize() );
						pDocinfo += iDocinfoStride;
					}
				} else
				{
					// we're not inlining, so only flush hits, docs are flushed independently
					dHitBlocks.Add ( tHitBuilder.cidxWriteRawVLB ( fdHits.GetFD(), dHits.Begin(), iHits,
						NULL, 0, 0 ) );
				}
				m_pDict->HitblockReset ();

				if ( dHitBlocks.Last()<0 )
					return 0;

				// progress bar
				m_tProgress.m_iHitsTotal += iHits;
				m_tProgress.m_iDocuments = m_tStats.m_iTotalDocuments + pSource->GetStats().m_iTotalDocuments;
				m_tProgress.m_iBytes = m_tStats.m_iTotalBytes + pSource->GetStats().m_iTotalBytes;
				m_tProgress.Show ( false );
			}

			// update min docinfo
			assert ( pSource->m_tDocInfo.m_iDocID );
			m_iMinDocid = Min ( m_iMinDocid, pSource->m_tDocInfo.m_iDocID );
			if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE )
			{
				ARRAY_FOREACH ( i, m_dMinRow )
					m_dMinRow[i] = Min ( m_dMinRow[i], pSource->m_tDocInfo.m_pDynamic[i] );
			}

			// update total field lengths
			if ( iFieldLens>=0 )
			{
				ARRAY_FOREACH ( i, m_tSchema.m_dFields )
					m_dFieldLens[i] += pSource->m_tDocInfo.GetAttr ( m_tSchema.GetAttr ( i+iFieldLens ).m_tLocator );
			}

			// store docinfo
			// with the advent of SPH_ATTR_TOKENCOUNT, now MUST be done AFTER iterating the hits
			// because field lengths are computed during that iterating
			if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN )
			{
				// store next entry
				DOCINFOSETID ( pDocinfo, pSource->m_tDocInfo.m_iDocID );

				// old docinfo found, use it instead of the new one
				const DWORD * pSrc = pPrevDocinfo ? DOCINFO2ATTRS ( pPrevDocinfo ) : pSource->m_tDocInfo.m_pDynamic;
				memcpy ( DOCINFO2ATTRS ( pDocinfo ), pSrc, sizeof(CSphRowitem)*m_tSchema.GetRowSize() );
				pDocinfo += iDocinfoStride;

				// if not inlining, flush buffer if it's full
				// (if inlining, it will flushed later, along with the hits)
				if ( pDocinfo>=pDocinfoMax )
				{
					assert ( pDocinfo==pDocinfoMax );
					int iLen = iDocinfoMax*iDocinfoStride*sizeof(DWORD);

					sphSortDocinfos ( dDocinfos.Begin(), iDocinfoMax, iDocinfoStride );
					if ( !sphWriteThrottled ( fdDocinfos.GetFD(), dDocinfos.Begin(), iLen, "raw_docinfos", m_sLastError, &g_tThrottle ) )
						return 0;

					pDocinfo = dDocinfos.Begin();
					iDocinfoBlocks++;
				}
			}

			// go on, loop next document
		}

		// FIXME! uncontrolled memory usage; add checks and/or diskbased sort in the future?
		if ( pSource->IterateKillListStart ( m_sLastError ) )
		{
			SphDocID_t tDocId;
			while ( pSource->IterateKillListNext ( tDocId ) )
				dKillList.Add ( tDocId );
		}

		// fetch joined fields
		if ( bGotJoined )
		{
			// flush tail of regular hits
			int iHits = pHits - dHits.Begin();
			if ( iDictSize && m_pDict->HitblockGetMemUse() && iHits )
			{
				sphSort ( dHits.Begin(), iHits, CmpHit_fn() );
				m_pDict->HitblockPatch ( dHits.Begin(), iHits );
				pHits = dHits.Begin();
				m_tProgress.m_iHitsTotal += iHits;
				dHitBlocks.Add ( tHitBuilder.cidxWriteRawVLB ( fdHits.GetFD(), dHits.Begin(), iHits, NULL, 0, 0 ) );
				if ( dHitBlocks.Last()<0 )
					return 0;
				m_pDict->HitblockReset ();
			}

			for ( ;; )
			{
				// get next doc, and handle errors
				ISphHits * pJoinedHits = pSource->IterateJoinedHits ( m_sLastError );
				if ( !pJoinedHits )
					return 0;

				// ensure docid is sane
				if ( pSource->m_tDocInfo.m_iDocID==DOCID_MAX )
				{
					m_sLastError.SetSprintf ( "joined_docid==DOCID_MAX (source broken?)" );
					return 0;
				}

				// check for eof
				if ( !pSource->m_tDocInfo.m_iDocID )
					break;

				int iJoinedHits = pJoinedHits->Length();
				memcpy ( pHits, pJoinedHits->First(), iJoinedHits*sizeof(CSphWordHit) );
				pHits += iJoinedHits;

				// check if we need to flush
				if ( pHits<pHitsMax && !( iDictSize && m_pDict->HitblockGetMemUse() > iDictSize ) )
					continue;

				// store hits
				int iHits = pHits - dHits.Begin();
				sphSort ( dHits.Begin(), iHits, CmpHit_fn() );
				m_pDict->HitblockPatch ( dHits.Begin(), iHits );

				pHits = dHits.Begin();
				m_tProgress.m_iHitsTotal += iHits;

				dHitBlocks.Add ( tHitBuilder.cidxWriteRawVLB ( fdHits.GetFD(), dHits.Begin(), iHits, NULL, 0, 0 ) );
				if ( dHitBlocks.Last()<0 )
					return 0;
				m_pDict->HitblockReset ();
			}
		}

		// this source is over, disconnect and update stats
		pSource->Disconnect ();

		m_tStats.m_iTotalDocuments += pSource->GetStats().m_iTotalDocuments;
		m_tStats.m_iTotalBytes += pSource->GetStats().m_iTotalBytes;
	}

	if ( m_tStats.m_iTotalDocuments>=INT_MAX )
	{
		m_sLastError.SetSprintf ( "index over %d documents not supported (got documents count="INT64_FMT")", INT_MAX, m_tStats.m_iTotalDocuments );
		return 0;
	}

	// flush last docinfo block
	int iDocinfoLastBlockSize = 0;
	if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN && pDocinfo>dDocinfos.Begin() )
	{
		iDocinfoLastBlockSize = ( pDocinfo - dDocinfos.Begin() ) / iDocinfoStride;
		assert ( pDocinfo==( dDocinfos.Begin() + iDocinfoLastBlockSize*iDocinfoStride ) );

		int iLen = iDocinfoLastBlockSize*iDocinfoStride*sizeof(DWORD);
		sphSortDocinfos ( dDocinfos.Begin(), iDocinfoLastBlockSize, iDocinfoStride );
		if ( !sphWriteThrottled ( fdDocinfos.GetFD(), dDocinfos.Begin(), iLen, "raw_docinfos", m_sLastError, &g_tThrottle ) )
			return 0;

		iDocinfoBlocks++;
	}

	// flush last hit block
	if ( pHits>dHits.Begin() )
	{
		int iHits = pHits - dHits.Begin();
		{
			sphSort ( dHits.Begin(), iHits, CmpHit_fn() );
			m_pDict->HitblockPatch ( dHits.Begin(), iHits );
		}
		m_tProgress.m_iHitsTotal += iHits;

		if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE )
		{
			int iDocs = ( pDocinfo - dDocinfos.Begin() ) / iDocinfoStride;
			sphSortDocinfos ( dDocinfos.Begin(), iDocs, iDocinfoStride );
			dHitBlocks.Add ( tHitBuilder.cidxWriteRawVLB ( fdHits.GetFD(), dHits.Begin(), iHits,
				dDocinfos.Begin(), iDocs, iDocinfoStride ) );
		} else
		{
			dHitBlocks.Add ( tHitBuilder.cidxWriteRawVLB ( fdHits.GetFD(), dHits.Begin(), iHits, NULL, 0, 0 ) );
		}
		m_pDict->HitblockReset ();

		if ( dHitBlocks.Last()<0 )
			return 0;
	}

	// flush last field MVA block
	if ( bHaveFieldMVAs && dFieldMVAs.GetLength () )
	{
		int iLength = dFieldMVAs.GetLength ();
		nFieldMVAs += iLength;

		dFieldMVAs.Sort ( CmpMvaEntries_fn () );
		if ( !sphWriteThrottled ( fdTmpFieldMVAs.GetFD (), &dFieldMVAs[0],
			iLength*sizeof(MvaEntry_t), "temp_field_mva", m_sLastError, &g_tThrottle ) )
				return 0;

		dFieldMVAs.Reset ();
	}

	// flush last ordinals block
	if ( bHaveOrdinals && dOrdinals[0].GetLength () )
	{
		nOrdinals += dOrdinals[0].GetLength ();

		ARRAY_FOREACH ( i, dOrdinalAttrs )
		{
			CSphVector<Ordinal_t> & dCol = dOrdinals[i];
			dCol.Sort ( CmpOrdinalsValue_fn() );

			SphOffset_t uSize = DumpOrdinals ( tOrdWriter, dCol );
			if ( !uSize )
			{
				m_sLastError = "dump ordinals: io error";
				return 0;
			}

			if ( uSize > uMaxOrdinalAttrBlockSize )
				uMaxOrdinalAttrBlockSize = uSize;

			dOrdBlockSize[i].Add ( uSize );
			dCol.Reset ();
		}
	}

	m_tProgress.m_iDocuments = m_tStats.m_iTotalDocuments;
	m_tProgress.m_iBytes = m_tStats.m_iTotalBytes;
	m_tProgress.Show ( true );

	///////////////////////////////////////
	// collect and sort multi-valued attrs
	///////////////////////////////////////
	if ( !BuildMVA ( dSources, dHits, iHitsMax*sizeof(CSphWordHit), fdTmpFieldMVAs.GetFD (), nFieldMVAs, iMaxPoolFieldMVAs, pPrevIndex.Ptr() ) )
		return 0;

	// reset persistent mva update pool
	::unlink ( GetIndexFileName("mvp").cstr() );

	// reset hits pool
	dHits.Reset ( 0 );

	CSphString sFieldMVAFile = fdTmpFieldMVAs.GetFilename ();
	fdTmpFieldMVAs.Close ();
	::unlink ( sFieldMVAFile.cstr () );

	/////////////////
	// sort docinfos
	/////////////////

	tOrdWriter.CloseFile ();
	if ( tOrdWriter.IsError () )
		return 0;

	CSphString sSortedOrdinalIdFile = GetIndexFileName("tmp6");

	// sort ordinals
	if ( bHaveOrdinals && !dOrdBlockSize[0].GetLength () )
	{
		bHaveOrdinals = false;
		::unlink ( sRawOrdinalsFile.cstr () );
	}

	if ( bHaveOrdinals )
	{
		if ( iMaxOrdLen > MAX_ORDINAL_STR_LEN )
			sphWarn ( "some ordinal attributes are too long (len=%d,max=%d)", iMaxOrdLen, MAX_ORDINAL_STR_LEN );

		CSphString sUnsortedIdFile = GetIndexFileName("tmp5");

		CSphAutofile fdRawOrdinals ( sRawOrdinalsFile.cstr (), SPH_O_READ, m_sLastError, true );
		if ( fdRawOrdinals.GetFD () < 0 )
			return 0;

		const float ARENA_PERCENT = 0.5f;
		int nBlocks = dOrdBlockSize[0].GetLength ();

		SphOffset_t uMemNeededForReaders = SphOffset_t ( nBlocks ) * uMaxOrdinalAttrBlockSize;
		SphOffset_t uMemNeededForSorting = sizeof ( OrdinalId_t ) * nOrdinals;

		int iArenaSize = (int) Min ( SphOffset_t ( iMemoryLimit * ARENA_PERCENT ), uMemNeededForReaders );
		iArenaSize = Max ( CSphBin::MIN_SIZE * nBlocks, iArenaSize );

		int iOrdinalsInPool = (int)Min ( (SphOffset_t)( iMemoryLimit*( 1.0f-ARENA_PERCENT ) ), uMemNeededForSorting )/sizeof(OrdinalId_t);

		if ( !SortOrdinals ( sUnsortedIdFile.cstr (), fdRawOrdinals.GetFD(),
			iArenaSize, iOrdinalsInPool, dOrdBlockSize, iArenaSize < uMemNeededForReaders ) )
				return 0;

		CSphAutofile fdUnsortedId ( sUnsortedIdFile.cstr (), SPH_O_READ, m_sLastError, true );
		if ( fdUnsortedId.GetFD () < 0 )
			return 0;

		iArenaSize = Min ( iMemoryLimit, (int)uMemNeededForSorting );
		iArenaSize = Max ( CSphBin::MIN_SIZE * ( nOrdinals / iOrdinalsInPool + 1 ), iArenaSize );

		if ( !SortOrdinalIds ( sSortedOrdinalIdFile.cstr (), fdUnsortedId.GetFD(),
			iArenaSize, dOrdBlockSize, iArenaSize < uMemNeededForSorting ) )
			return 0;
	}

	// initialize MVA reader
	CSphAutoreader rdMva;
	if ( !rdMva.Open ( GetIndexFileName("spm"), m_sLastError ) )
		return 0;

	SphDocID_t uMvaID = rdMva.GetDocid();

	// initialize writer
	int iDocinfoFD = -1;
	SphOffset_t iDocinfoWritePos = 0;
	CSphScopedPtr<CSphAutofile> pfdDocinfoFinal ( NULL );

	if ( m_bInplaceSettings )
		iDocinfoFD = fdDocinfos.GetFD ();
	else
	{
		pfdDocinfoFinal = new CSphAutofile ( GetIndexFileName("spa"), SPH_O_NEW, m_sLastError );
		iDocinfoFD = pfdDocinfoFinal->GetFD();
		if ( iDocinfoFD < 0 )
			return 0;
	}

	int iDupes = 0;
	int iMinBlock = -1;

	if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN && iDocinfoBlocks )
	{
		// initialize readers
		assert ( dBins.GetLength()==0 );
		dBins.Reserve ( iDocinfoBlocks );

		float fReadFactor = 1.0f;
		float fRelocFactor = 0.0f;
		if ( m_bInplaceSettings )
		{
			assert ( m_fRelocFactor > 0.005f && m_fRelocFactor < 0.95f );
			fRelocFactor = m_fRelocFactor;
			fReadFactor -= fRelocFactor;
		}

		int iBinSize = CSphBin::CalcBinSize ( int ( iMemoryLimit * fReadFactor ), iDocinfoBlocks, "sort_docinfos" );
		int iRelocationSize = m_bInplaceSettings ? int ( iMemoryLimit * fRelocFactor ) : 0;
		CSphFixedVector<BYTE> dRelocationBuffer ( iRelocationSize );
		iSharedOffset = -1;

		for ( int i=0; i<iDocinfoBlocks; i++ )
		{
			dBins.Add ( new CSphBin() );
			dBins[i]->m_iFileLeft = ( ( i==iDocinfoBlocks-1 ) ? iDocinfoLastBlockSize : iDocinfoMax )*iDocinfoStride*sizeof(DWORD);
			dBins[i]->m_iFilePos = ( i==0 ) ? iDocinfosGap : dBins[i-1]->m_iFilePos + dBins[i-1]->m_iFileLeft;
			dBins[i]->Init ( fdDocinfos.GetFD(), &iSharedOffset, iBinSize );
		}

		SphOffset_t iDocinfoFileSize = 0;
		if ( iDocinfoBlocks )
			iDocinfoFileSize = dBins [iDocinfoBlocks-1]->m_iFilePos + dBins [iDocinfoBlocks-1]->m_iFileLeft;

		// docinfo queue
		CSphFixedVector<DWORD> dDocinfoQueue ( iDocinfoBlocks*iDocinfoStride );
		CSphQueue < int, CmpQueuedDocinfo_fn > qDocinfo ( iDocinfoBlocks );

		CmpQueuedDocinfo_fn::m_pStorage = dDocinfoQueue.Begin();
		CmpQueuedDocinfo_fn::m_iStride = iDocinfoStride;

		pDocinfo = dDocinfoQueue.Begin();
		for ( int i=0; i<iDocinfoBlocks; i++ )
		{
			if ( dBins[i]->ReadBytes ( pDocinfo, iDocinfoStride*sizeof(DWORD) )!=BIN_READ_OK )
			{
				m_sLastError.SetSprintf ( "sort_docinfos: warmup failed (io error?)" );
				return 0;
			}
			pDocinfo += iDocinfoStride;
			qDocinfo.Push ( i );
		}

		CSphVector < CSphBin > dOrdReaders;
		SphOffset_t iSharedOrdOffset = -1;

		CSphAutofile fdTmpSortedIds ( sSortedOrdinalIdFile.cstr (), SPH_O_READ, m_sLastError, true );

		if ( bHaveOrdinals )
		{
			if ( fdTmpSortedIds.GetFD () < 0 )
				return 0;

			dOrdReaders.Resize ( dOrdinalAttrs.GetLength () );
			SphOffset_t uStart = 0;
			ARRAY_FOREACH ( i, dOrdReaders )
			{
				dOrdReaders[i].m_iFileLeft = (int)dOrdBlockSize [i][0];
				dOrdReaders[i].m_iFilePos = uStart;
				dOrdReaders[i].Init ( fdTmpSortedIds.GetFD(), &iSharedOrdOffset, ORDINAL_READ_SIZE );
				uStart += dOrdReaders[i].m_iFileLeft;
			}
		}

		// while the queue has data for us
		int iOrd = 0;
		pDocinfo = dDocinfos.Begin();
		SphDocID_t uLastId = 0;
		m_uMinMaxIndex = 0;

		// prepare the collector for min/max of attributes
		AttrIndexBuilder_c tMinMax ( m_tSchema );
		int64_t iMinMaxSize = tMinMax.GetExpectedSize ( m_tStats.m_iTotalDocuments );
		if ( iMinMaxSize>INT_MAX || m_tStats.m_iTotalDocuments>INT_MAX )
		{
			m_sLastError.SetSprintf ( "attribute files (.spa) over 128 GB are not supported (min-max approximate="INT64_FMT", documents count="INT64_FMT")",
				iMinMaxSize, m_tStats.m_iTotalDocuments );
			return 0;
		}
		CSphFixedVector<DWORD> dMinMaxBuffer ( (int)iMinMaxSize );
		memset ( dMinMaxBuffer.Begin(), 0, (int)iMinMaxSize*sizeof(DWORD) );

		// { fixed row + dummy value ( zero offset elimination ) + mva data for that row } fixed row - for MinMaxBuilder
		CSphVector < DWORD > dMvaPool;
		tMinMax.Prepare ( dMinMaxBuffer.Begin(), dMinMaxBuffer.Begin() + dMinMaxBuffer.GetLength() ); // FIXME!!! for over INT_MAX blocks

		// the last (or, lucky, the only, string chunk)
		dStringChunks.Add ( DWORD(tStrWriter.GetPos()-uStringChunk) );

		tStrWriter.CloseFile();
		if ( !dStringAttrs.GetLength() )
			::unlink ( GetIndexFileName("tmps").cstr() );

		SphDocID_t uLastDupe = 0;
		while ( qDocinfo.GetLength() )
		{
			// obtain bin index and next entry
			int iBin = qDocinfo.Root();
			DWORD * pEntry = dDocinfoQueue.Begin() + iBin*iDocinfoStride;

			if ( DOCINFO2ID ( pEntry )<uLastId )
			{
				m_sLastError.SetSprintf ( "descending document prev id="DOCID_FMT", curr="DOCID_FMT" bin=%d", uLastId, DOCINFO2ID ( pEntry ), iBin );
				return 0;
			}

			// skip duplicates
			if ( DOCINFO2ID ( pEntry )==uLastId )
			{
				// dupe, report it
				if ( m_tSettings.m_bVerbose && uLastDupe!=uLastId )
					sphWarn ( "duplicated document id="DOCID_FMT, uLastId );

				uLastDupe = uLastId;
				iDupes++;

			} else
			{
				// new unique document, handle it
				// update ordinals
				ARRAY_FOREACH ( i, dOrdinalAttrs )
				{
					OrdinalId_t Id;
					if ( dOrdReaders[i].ReadBytes ( &Id, sizeof(Id) )!=BIN_READ_OK )
					{
						m_sLastError = "update ordinals: io error";
						return 0;
					}

					assert ( Id.m_uDocID==DOCINFO2ID(pEntry) );
					sphSetRowAttr ( DOCINFO2ATTRS(pEntry), m_tSchema.GetAttr(dOrdinalAttrs[i]).m_tLocator, Id.m_uId );
				}
				iOrd++;
				m_uMinMaxIndex += iDocinfoStride;

				CSphRowitem * pCollectibleRow = pEntry;
				// update MVA
				if ( bGotMVA )
				{
					// go to next id
					while ( uMvaID<DOCINFO2ID(pEntry) )
					{
						ARRAY_FOREACH ( i, dMvaIndexes )
						{
							int iCount = rdMva.GetDword();
							rdMva.SkipBytes ( iCount*sizeof(DWORD) );
						}

						uMvaID = rdMva.GetDocid();
						if ( !uMvaID )
							uMvaID = DOCID_MAX;
					}

					assert ( uMvaID>=DOCINFO2ID(pEntry) );
					if ( uMvaID==DOCINFO2ID(pEntry) )
					{
						// fixed row + dummy value ( zero offset elemination )
						dMvaPool.Resize ( iDocinfoStride+1 );
						memcpy ( dMvaPool.Begin(), pEntry, iDocinfoStride * sizeof(DWORD) );

						CSphRowitem * pAttr = DOCINFO2ATTRS ( pEntry );
						ARRAY_FOREACH ( i, dMvaIndexes )
						{
							SphOffset_t iMvaOff = rdMva.GetPos()/sizeof(DWORD);
							assert ( iMvaOff<UINT_MAX );
							int iPoolOff = dMvaPool.GetLength();

							sphSetRowAttr ( pAttr, dMvaLocators[i], iMvaOff );
							// there is the cloned row at the beginning of MVA pool, lets skip it
							sphSetRowAttr ( dMvaPool.Begin()+DOCINFO_IDSIZE, dMvaLocators[i], iPoolOff - iDocinfoStride );

							DWORD iMvaCount = rdMva.GetDword();
							dMvaPool.Resize ( iPoolOff+iMvaCount+1 );
							dMvaPool[iPoolOff] = iMvaCount;
							rdMva.GetBytes ( dMvaPool.Begin()+iPoolOff+1, sizeof(DWORD)*iMvaCount );
						}
						pCollectibleRow = dMvaPool.Begin();

						uMvaID = rdMva.GetDocid();
						if ( !uMvaID )
							uMvaID = DOCID_MAX;
					}
				}

				if ( !tMinMax.Collect ( pCollectibleRow, dMvaPool.Begin()+iDocinfoStride, dMvaPool.GetLength()-iDocinfoStride, m_sLastError, false ) )
					return 0;
				dMvaPool.Resize ( iDocinfoStride );

				// emit it
				memcpy ( pDocinfo, pEntry, iDocinfoStride*sizeof(DWORD) );
				pDocinfo += iDocinfoStride;
				uLastId = DOCINFO2ID(pEntry);

				if ( pDocinfo>=pDocinfoMax )
				{
					int iLen = iDocinfoMax*iDocinfoStride*sizeof(DWORD);

					if ( m_bInplaceSettings )
					{
						if ( iMinBlock==-1 || dBins[iMinBlock]->IsEOF () )
						{
							iMinBlock = -1;
							ARRAY_FOREACH ( i, dBins )
								if ( !dBins[i]->IsEOF () && ( iMinBlock==-1 || dBins [i]->m_iFilePos<dBins [iMinBlock]->m_iFilePos ) )
									iMinBlock = i;
						}

						if ( iMinBlock!=-1 && ( iDocinfoWritePos + iLen ) > dBins[iMinBlock]->m_iFilePos )
						{
							if ( !RelocateBlock ( iDocinfoFD, dRelocationBuffer.Begin(), iRelocationSize, &iDocinfoFileSize, dBins[iMinBlock], &iSharedOffset ) )
								return 0;

							iMinBlock = (iMinBlock+1) % dBins.GetLength ();
						}

						sphSeek ( iDocinfoFD, iDocinfoWritePos, SEEK_SET );
						iSharedOffset = iDocinfoWritePos;
					}

					if ( !sphWriteThrottled ( iDocinfoFD, dDocinfos.Begin(), iLen, "sort_docinfo", m_sLastError, &g_tThrottle ) )
						return 0;

					iDocinfoWritePos += iLen;
					pDocinfo = dDocinfos.Begin();
				}
			}

			// pop its index, update it, push its index again
			qDocinfo.Pop ();
			ESphBinRead eRes = dBins[iBin]->ReadBytes ( pEntry, iDocinfoStride*sizeof(DWORD) );
			if ( eRes==BIN_READ_ERROR )
			{
				m_sLastError.SetSprintf ( "sort_docinfo: failed to read entry" );
				return 0;
			}
			if ( eRes==BIN_READ_OK )
				qDocinfo.Push ( iBin );
		}

		if ( pDocinfo>dDocinfos.Begin() )
		{
			assert ( 0==( pDocinfo-dDocinfos.Begin() ) % iDocinfoStride );
			int iLen = ( pDocinfo - dDocinfos.Begin() )*sizeof(DWORD);

			if ( m_bInplaceSettings )
				sphSeek ( iDocinfoFD, iDocinfoWritePos, SEEK_SET );

			if ( !sphWriteThrottled ( iDocinfoFD, dDocinfos.Begin(), iLen, "sort_docinfo", m_sLastError, &g_tThrottle ) )
				return 0;

			if ( m_bInplaceSettings )
				if ( !sphTruncate ( iDocinfoFD ) )
					sphWarn ( "failed to truncate %s", fdDocinfos.GetFilename() );

			iDocinfoWritePos += iLen;
		}
		tMinMax.FinishCollect();
		int64_t iMinMaxRealSize = tMinMax.GetActualSize() * sizeof(DWORD);
		if ( !sphWriteThrottled ( iDocinfoFD, dMinMaxBuffer.Begin(), iMinMaxRealSize, "minmax_docinfo", m_sLastError, &g_tThrottle ) )
				return 0;

		// clean up readers
		ARRAY_FOREACH ( i, dBins )
			SafeDelete ( dBins[i] );

		dBins.Reset ();
	}

	dDocinfos.Reset ( 0 );
	pDocinfo = NULL;

	// iDocinfoWritePos now contains the true size of pure attributes (without block indexes) in bytes
	int iStringStride = dStringAttrs.GetLength();
	SphOffset_t iNumDocs = iDocinfoWritePos/sizeof(DWORD)/iDocinfoStride;
	CSphTightVector<DWORD> dStrOffsets;

	if ( iStringStride )
	{
		// read only non-zero string locators
		{
			CSphReader tAttrReader;
			tAttrReader.SetFile ( iDocinfoFD, GetIndexFileName ( "spa" ).cstr() );
			CSphFixedVector<DWORD> dDocinfo ( iDocinfoStride );
			pDocinfo = dDocinfo.Begin();
			for ( SphOffset_t i=0; i<iNumDocs; ++i )
			{
				tAttrReader.GetBytes ( pDocinfo, iDocinfoStride*sizeof(DWORD) );
				CSphRowitem * pAttrs = DOCINFO2ATTRS ( pDocinfo );
				ARRAY_FOREACH ( j, dStringAttrs )
				{
					const CSphAttrLocator & tLoc = m_tSchema.GetAttr ( dStringAttrs[j] ).m_tLocator;
					DWORD uData = (DWORD)sphGetRowAttr ( pAttrs, tLoc );
					if ( uData )
						dStrOffsets.Add ( uData );
				}
			}
		} // the spa reader eliminates out of this scope
		DWORD iNumStrings = dStrOffsets.GetLength();

		// reopen strings for reading
		CSphAutofile tRawStringsFile;
		CSphReader tStrReader;
		if ( tRawStringsFile.Open ( GetIndexFileName("tmps"), SPH_O_READ, m_sLastError, true )<0 )
			return 0;
		tStrReader.SetFile ( tRawStringsFile );

		// now just load string chunks and resort them...
		CSphFixedVector<BYTE> dStringPool ( iPoolSize );
		BYTE* pStringsBegin = dStringPool.Begin();

		// if we have more than 1 string chunks, we need several passes and bitmask to distinquish them
		if ( dStringChunks.GetLength()>1 )
		{
			dStrOffsets.Resize ( iNumStrings+(iNumStrings>>5)+1 );
			DWORD* pDocinfoBitmap = &dStrOffsets[iNumStrings];
			for ( DWORD i=0; i<1+(iNumStrings>>5); ++i )
				pDocinfoBitmap[i] = 0;
			SphOffset_t iMinStrings = 0;

			ARRAY_FOREACH ( i, dStringChunks )
			{
				// read the current chunk
				SphOffset_t iMaxStrings = iMinStrings + dStringChunks[i];
				tStrReader.GetBytes ( pStringsBegin, dStringChunks[i] );

				// walk throw the attributes and put the strings in the new order
				DWORD uMaskOff = 0;
				DWORD uMask = 1;
				for ( DWORD k=0; k<iNumStrings; ++k )
				{
					if ( uMask==0x80000000 )
					{
						uMask = 1;
						++uMaskOff;
					} else
						uMask <<= 1;
					DWORD& uCurStr = dStrOffsets[k];
					// already processed, or hit out of the the current chunk?
					if ( pDocinfoBitmap[uMaskOff]&uMask || !uCurStr || uCurStr<iMinStrings || uCurStr>=iMaxStrings )
						continue;

					const BYTE * pStr = NULL;
					int iLen = sphUnpackStr ( pStringsBegin + uCurStr - iMinStrings, &pStr );
					if ( !iLen )
						uCurStr = 0;
					else
					{
						uCurStr = (DWORD)tStrFinalWriter.GetPos();
						BYTE dPackedLen[4];
						int iLenLen = sphPackStrlen ( dPackedLen, iLen );
						tStrFinalWriter.PutBytes ( &dPackedLen, iLenLen );
						tStrFinalWriter.PutBytes ( pStr, iLen );
					}
					pDocinfoBitmap[uMaskOff]|=uMask;
				}
				iMinStrings = iMaxStrings;
			}
		} else if ( dStringChunks.GetLength()==1 ) // only one chunk. Plain and simple!
		{
			DWORD iStringChunk = dStringChunks[0];
			tStrReader.GetBytes ( pStringsBegin, iStringChunk );

			// walk throw the attributes and put the strings in the new order
			for ( DWORD k=0; k<iNumStrings; ++k )
			{
				DWORD& uOffset = dStrOffsets[k];
				// already processed, or hit out of the the current chunk?
				if ( uOffset<1 || uOffset>=iStringChunk )
					continue;

				const BYTE * pStr = NULL;
				int iLen = sphUnpackStr ( pStringsBegin + uOffset, &pStr );
				if ( !iLen )
					uOffset = 0;
				else
				{
					uOffset = (DWORD)tStrFinalWriter.GetPos();
					BYTE dPackedLen[4];
					int iLenLen = sphPackStrlen ( dPackedLen, iLen );
					tStrFinalWriter.PutBytes ( &dPackedLen, iLenLen );
					tStrFinalWriter.PutBytes ( pStr, iLen );
				}
			}
		}
		dStringPool.Reset(0);
		// now save back patched string locators
		{
			DWORD iDocPoolSize = iPoolSize/iDocinfoStride/sizeof(DWORD);
			CSphFixedVector<DWORD> dDocinfoPool ( iDocPoolSize*iDocinfoStride );
			pDocinfo = dDocinfoPool.Begin();
			DWORD iToRead = Min ( iDocPoolSize, DWORD(iNumDocs) );
			SphOffset_t iPos = 0;
			DWORD iStr = 0;
			while ( iToRead )
			{
				sphSeek ( iDocinfoFD, iPos, SEEK_SET );
				sphRead ( iDocinfoFD, pDocinfo, iToRead*iDocinfoStride*sizeof(DWORD));
				for ( DWORD i=0; i<iToRead; ++i )
				{
					CSphRowitem * pAttrs = DOCINFO2ATTRS ( pDocinfo+i*iDocinfoStride );
					ARRAY_FOREACH ( j, dStringAttrs )
					{
						const CSphAttrLocator& tLocator = m_tSchema.GetAttr ( dStringAttrs[j] ).m_tLocator;
						if ( sphGetRowAttr ( pAttrs, tLocator ) )
							sphSetRowAttr ( pAttrs, tLocator, dStrOffsets[iStr++] );
					}
				}
				sphSeek ( iDocinfoFD, iPos, SEEK_SET );
				sphWrite ( iDocinfoFD, pDocinfo, iToRead*iDocinfoStride*sizeof(DWORD));
				iPos+=iToRead*iDocinfoStride*sizeof(DWORD);
				iNumDocs-=iToRead;
				iToRead = Min ( iDocPoolSize, DWORD(iNumDocs) );
			}
		} // all temporary buffers eliminates out of this scope
	}

	// it might be zero-length, but it must exist
	if ( m_bInplaceSettings )
		fdDocinfos.Close ();
	else
	{
		assert ( pfdDocinfoFinal.Ptr () );
		pfdDocinfoFinal->Close ();
	}

	// dump killlist
	CSphAutofile fdKillList ( GetIndexFileName("spk"), SPH_O_NEW, m_sLastError );
	if ( fdKillList.GetFD()<0 )
		return 0;

	if ( dKillList.GetLength () )
	{
		dKillList.Uniq ();

		m_iKillListSize = dKillList.GetLength ();

		if ( !sphWriteThrottled ( fdKillList.GetFD(), &dKillList[0],
			m_iKillListSize*sizeof(SphAttr_t), "kill list", m_sLastError, &g_tThrottle ) )
				return 0;
	}

	fdKillList.Close ();

	///////////////////////////////////
	// sort and write compressed index
	///////////////////////////////////

	// initialize readers
	assert ( dBins.GetLength()==0 );
	dBins.Reserve ( dHitBlocks.GetLength() );

	iSharedOffset = -1;

	float fReadFactor = 1.0f;
	int iRelocationSize = 0;
	iWriteBuffer = iHitBuilderBufferSize;

	if ( m_bInplaceSettings )
	{
		assert ( m_fRelocFactor > 0.005f && m_fRelocFactor < 0.95f );
		assert ( m_fWriteFactor > 0.005f && m_fWriteFactor < 0.95f );
		assert ( m_fWriteFactor+m_fRelocFactor < 1.0f );

		fReadFactor -= m_fRelocFactor + m_fWriteFactor;

		iRelocationSize = int ( iMemoryLimit * m_fRelocFactor );
		iWriteBuffer = int ( iMemoryLimit * m_fWriteFactor );
	}

	int iBinSize = CSphBin::CalcBinSize ( int ( iMemoryLimit * fReadFactor ),
		dHitBlocks.GetLength() + m_pDict->GetSettings().m_bWordDict, "sort_hits" );

	CSphFixedVector <BYTE> dRelocationBuffer ( iRelocationSize );
	iSharedOffset = -1;

	ARRAY_FOREACH ( i, dHitBlocks )
	{
		dBins.Add ( new CSphBin ( m_tSettings.m_eHitless, m_pDict->GetSettings().m_bWordDict ) );
		dBins[i]->m_iFileLeft = dHitBlocks[i];
		dBins[i]->m_iFilePos = ( i==0 ) ? iHitsGap : dBins[i-1]->m_iFilePos + dBins[i-1]->m_iFileLeft;
		dBins[i]->Init ( fdHits.GetFD(), &iSharedOffset, iBinSize );
	}

	// if there were no hits, create zero-length index files
	int iRawBlocks = dBins.GetLength();

	//////////////////////////////
	// create new index files set
	//////////////////////////////

	tHitBuilder.CreateIndexFiles ( GetIndexFileName("spd").cstr(), GetIndexFileName("spp").cstr(),
		GetIndexFileName("spe").cstr(), m_bInplaceSettings, iWriteBuffer, fdHits, &iSharedOffset );

	// dict files
	CSphAutofile fdTmpDict ( GetIndexFileName("tmp8"), SPH_O_NEW, m_sLastError, true );
	CSphAutofile fdDict ( GetIndexFileName("spi"), SPH_O_NEW, m_sLastError, false );
	if ( fdTmpDict.GetFD()<0 || fdDict.GetFD()<0 )
		return 0;
	m_pDict->DictBegin ( fdTmpDict, fdDict, iBinSize, &g_tThrottle );

	// adjust min IDs, and fill header
	assert ( m_iMinDocid>0 );
	m_iMinDocid--;
	if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE )
		ARRAY_FOREACH ( i, m_dMinRow )
			m_dMinRow[i]--;

	tHitBuilder.SetMin ( m_dMinRow.Begin(), m_dMinRow.GetLength() );

	//////////////
	// final sort
	//////////////

	if ( iRawBlocks )
	{
		int iLastBin = dBins.GetLength () - 1;
		SphOffset_t iHitFileSize = dBins[iLastBin]->m_iFilePos + dBins [iLastBin]->m_iFileLeft;

		CSphHitQueue tQueue ( iRawBlocks );
		CSphAggregateHit tHit;

		// initialize hitlist encoder state
		tHitBuilder.HitReset();

		// initial fill
		int iRowitems = ( m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE ) ? m_tSchema.GetRowSize() : 0;
		CSphFixedVector<CSphRowitem> dInlineAttrs ( iRawBlocks*iRowitems );

		CSphFixedVector<BYTE> dActive ( iRawBlocks );
		for ( int i=0; i<iRawBlocks; i++ )
		{
			if ( !dBins[i]->ReadHit ( &tHit, iRowitems, dInlineAttrs.Begin() + i * iRowitems ) )
			{
				m_sLastError.SetSprintf ( "sort_hits: warmup failed (io error?)" );
				return 0;
			}
			dActive[i] = ( tHit.m_iWordID!=0 );
			if ( dActive[i] )
				tQueue.Push ( tHit, i );
		}

		// init progress meter
		m_tProgress.m_ePhase = CSphIndexProgress::PHASE_SORT;
		m_tProgress.m_iHits = 0;

		// while the queue has data for us
		// FIXME! analyze binsRead return code
		int iHitsSorted = 0;
		iMinBlock = -1;
		while ( tQueue.m_iUsed )
		{
			int iBin = tQueue.m_pData->m_iBin;

			// pack and emit queue root
			tQueue.m_pData->m_iDocID -= m_iMinDocid;

			if ( m_bInplaceSettings )
			{
				if ( iMinBlock==-1 || dBins[iMinBlock]->IsEOF () || !dActive[iMinBlock] )
				{
					iMinBlock = -1;
					ARRAY_FOREACH ( i, dBins )
						if ( !dBins[i]->IsEOF () && dActive[i] && ( iMinBlock==-1 || dBins[i]->m_iFilePos < dBins[iMinBlock]->m_iFilePos ) )
							iMinBlock = i;
				}

				int iToWriteMax = 3*sizeof(DWORD);
				if ( iMinBlock!=-1 && ( tHitBuilder.GetHitfilePos() + iToWriteMax ) > dBins[iMinBlock]->m_iFilePos )
				{
					if ( !RelocateBlock ( fdHits.GetFD (), dRelocationBuffer.Begin(), iRelocationSize, &iHitFileSize, dBins[iMinBlock], &iSharedOffset ) )
						return 0;

					iMinBlock = (iMinBlock+1) % dBins.GetLength ();
				}
			}

			tHitBuilder.cidxHit ( tQueue.m_pData, iRowitems ? dInlineAttrs.Begin() + iBin * iRowitems : NULL );
			if ( tHitBuilder.IsError() )
				return 0;

			// pop queue root and push next hit from popped bin
			tQueue.Pop ();
			if ( dActive[iBin] )
			{
				dBins[iBin]->ReadHit ( &tHit, iRowitems, dInlineAttrs.Begin() + iBin * iRowitems );
				dActive[iBin] = ( tHit.m_iWordID!=0 );
				if ( dActive[iBin] )
					tQueue.Push ( tHit, iBin );
			}

			// progress
			if ( ++iHitsSorted==1000000 )
			{
				m_tProgress.m_iHits += iHitsSorted;
				m_tProgress.Show ( false );
				iHitsSorted = 0;
			}
		}

		m_tProgress.m_iHits = m_tProgress.m_iHitsTotal; // sum might be less than total because of dupes!
		m_tProgress.Show ( true );

		ARRAY_FOREACH ( i, dBins )
			SafeDelete ( dBins[i] );
		dBins.Reset ();

		CSphAggregateHit tFlush;
		tFlush.m_iDocID = 0;
		tFlush.m_iWordID = 0;
		tFlush.m_sKeyword = NULL;
		tFlush.m_iWordPos = EMPTY_HIT;
		tFlush.m_dFieldMask.UnsetAll();
		tHitBuilder.cidxHit ( &tFlush, NULL );

		if ( m_bInplaceSettings )
		{
			tHitBuilder.CloseHitlist();
			if ( !sphTruncate ( fdHits.GetFD () ) )
				sphWarn ( "failed to truncate %s", fdHits.GetFilename() );
		}
	}

	if ( iDupes )
		sphWarn ( "%d duplicate document id pairs found", iDupes );

	BuildHeader_t tBuildHeader ( m_tStats );
	if ( !tHitBuilder.cidxDone ( iMemoryLimit, m_tSettings.m_iMinInfixLen, m_pTokenizer->GetMaxCodepointLength(), &tBuildHeader ) )
		return 0;

	tBuildHeader.m_sHeaderExtension = "sph";
	tBuildHeader.m_pMinRow = m_dMinRow.Begin();
	tBuildHeader.m_iMinDocid = m_iMinDocid;
	tBuildHeader.m_pThrottle = &g_tThrottle;
	tBuildHeader.m_iKillListSize = m_iKillListSize;
	tBuildHeader.m_uMinMaxIndex = m_uMinMaxIndex;
	tBuildHeader.m_iTotalDups = iDupes;

	// we're done
	if ( !BuildDone ( tBuildHeader, m_sLastError ) )
		return 0;

	// when the party's over..
	ARRAY_FOREACH ( i, dSources )
		dSources[i]->PostIndex ();

	dFileWatchdog.AllIsDone();
	return 1;
} // NOLINT function length


/////////////////////////////////////////////////////////////////////////////
// MERGER HELPERS
/////////////////////////////////////////////////////////////////////////////


static bool CopyFile ( const char * sSrc, const char * sDst, CSphString & sErrStr, ThrottleState_t * pThrottle, volatile bool * pForceTerminate )
{
	assert ( sSrc );
	assert ( sDst );

	const DWORD iMaxBufSize = 1024 * 1024;

	CSphAutofile tSrcFile ( sSrc, SPH_O_READ, sErrStr );
	CSphAutofile tDstFile ( sDst, SPH_O_NEW, sErrStr );

	if ( tSrcFile.GetFD()<0 || tDstFile.GetFD()<0 )
		return false;

	SphOffset_t iFileSize = tSrcFile.GetSize();
	DWORD iBufSize = (DWORD) Min ( iFileSize, (SphOffset_t)iMaxBufSize );

	if ( iFileSize )
	{
		CSphFixedVector<BYTE> dData ( iBufSize );
		bool bError = true;

		while ( iFileSize > 0 )
		{
			if ( *pForceTerminate )
				return false;

			DWORD iSize = (DWORD) Min ( iFileSize, (SphOffset_t)iBufSize );

			size_t iRead = sphReadThrottled ( tSrcFile.GetFD(), dData.Begin(), iSize, pThrottle );
			if ( iRead!=iSize )
			{
				sErrStr.SetSprintf ( "read error in %s; "INT64_FMT" of %d bytes read", sSrc, (int64_t)iRead, iSize );
				break;
			}

			if ( !sphWriteThrottled ( tDstFile.GetFD(), dData.Begin(), iSize, "CopyFile", sErrStr, pThrottle ) )
				break;

			iFileSize -= iSize;

			if ( !iFileSize )
				bError = false;
		}

		return ( bError==false );
	}

	return true;
}


static void CopyRowString ( const BYTE * pBase, const CSphVector<CSphAttrLocator> & dString, CSphRowitem * pRow, CSphWriter & wrTo )
{
	if ( !dString.GetLength() )
		return;

	CSphRowitem * pAttr = DOCINFO2ATTRS ( pRow );
	ARRAY_FOREACH ( i, dString )
	{
		SphAttr_t uOff = sphGetRowAttr ( pAttr, dString[i] );
		// magic offset? do nothing
		if ( !uOff )
			continue;

		const BYTE * pStr = NULL;
		int iLen = sphUnpackStr ( pBase + uOff, &pStr );

		// no data? do nothing
		if ( !iLen )
			continue;

		// copy bytes
		uOff = (SphAttr_t)wrTo.GetPos();
		assert ( uOff<UINT_MAX );
		sphSetRowAttr ( pAttr, dString[i], uOff );

		BYTE dPackedLen[4];
		int iLenLen = sphPackStrlen ( dPackedLen, iLen );
		wrTo.PutBytes ( &dPackedLen, iLenLen );
		wrTo.PutBytes ( pStr, iLen );
	}
}

static void CopyRowMVA ( const DWORD * pBase, const CSphVector<CSphAttrLocator> & dMva,
	SphDocID_t iDocid, CSphRowitem * pRow, CSphWriter & wrTo )
{
	if ( !dMva.GetLength() )
		return;

	CSphRowitem * pAttr = DOCINFO2ATTRS ( pRow );
	bool bDocidWriten = false;
	ARRAY_FOREACH ( i, dMva )
	{
		SphAttr_t uOff = sphGetRowAttr ( pAttr, dMva[i] );
		if ( !uOff )
			continue;

		assert ( pBase );
		if ( !bDocidWriten )
		{
			assert ( DOCINFO2ID ( pBase + uOff - DOCINFO_IDSIZE )==iDocid ); // there is DocID prior to 1st MVA
			wrTo.PutDocid ( iDocid );
			bDocidWriten = true;
		}

		assert ( wrTo.GetPos()/sizeof(DWORD)<=UINT_MAX );
		SphAttr_t uNewOff = ( DWORD )wrTo.GetPos() / sizeof( DWORD );
		sphSetRowAttr ( pAttr, dMva[i], uNewOff );

		DWORD iValues = pBase[uOff];
		wrTo.PutBytes ( pBase + uOff, ( iValues+1 )*sizeof(DWORD) );
	}
}


static const int DOCLIST_HINT_THRESH = 256;

// let uDocs be DWORD here to prevent int overflow in case of hitless word (highest bit is 1)
static int DoclistHintUnpack ( DWORD uDocs, BYTE uHint )
{
	if ( uDocs<(DWORD)DOCLIST_HINT_THRESH )
		return 8*uDocs;
	else
		return 4*uDocs + (DWORD)( uint64_t(uDocs)*uHint/64 );
}

BYTE sphDoclistHintPack ( SphOffset_t iDocs, SphOffset_t iLen )
{
	// we won't really store a hint for small lists
	if ( iDocs<DOCLIST_HINT_THRESH )
		return 0;

	// for bigger lists len/docs varies 4x-6x on test indexes
	// so lets assume that 4x-8x should be enough for everybody
	SphOffset_t iDelta = Min ( Max ( iLen-4*iDocs, 0 ), 4*iDocs-1 ); // len delta over 4x, clamped to [0x..4x) range
	BYTE uHint = (BYTE)( 64*iDelta/iDocs ); // hint now must be in [0..256) range
	while ( uHint<255 && ( iDocs*uHint/64 )<iDelta ) // roundoff (suddenly, my guru math skillz failed me)
		uHint++;

	return uHint;
}

// !COMMIT eliminate this, move to dict (or at least couple with CWordlist)
class CSphDictReader
{
public:
	// current word
	SphWordID_t		m_iWordID;
	SphOffset_t		m_iDoclistOffset;
	int				m_iDocs;
	int				m_iHits;
	bool			m_bHasHitlist;
	int				m_iHint;

private:
	ESphHitless		m_eHitless;
	CSphAutoreader	m_tMyReader;
	CSphReader *	m_pReader;
	SphOffset_t		m_iMaxPos;

	bool			m_bWordDict;
	char			m_sWord[MAX_KEYWORD_BYTES];

	int				m_iCheckpoint;
	bool			m_bHasSkips;

public:
	CSphDictReader()
		: m_iWordID ( 0 )
		, m_iDoclistOffset ( 0 )
		, m_iHint ( 0 )
		, m_iMaxPos ( 0 )
		, m_bWordDict ( true )
		, m_iCheckpoint ( 1 )
		, m_bHasSkips ( false )
	{
		m_sWord[0] = '\0';
	}

	bool Setup ( const CSphString & sFilename, SphOffset_t iMaxPos, ESphHitless eHitless,
		CSphString & sError, bool bWordDict, ThrottleState_t * pThrottle, bool bHasSkips )
	{
		if ( !m_tMyReader.Open ( sFilename, sError ) )
			return false;
		Setup ( &m_tMyReader, iMaxPos, eHitless, bWordDict, pThrottle, bHasSkips );
		return true;
	}

	void Setup ( CSphReader * pReader, SphOffset_t iMaxPos, ESphHitless eHitless, bool bWordDict, ThrottleState_t * pThrottle, bool bHasSkips )
	{
		m_pReader = pReader;
		m_pReader->SetThrottle ( pThrottle );
		m_pReader->SeekTo ( 1, READ_NO_SIZE_HINT );

		m_iMaxPos = iMaxPos;
		m_eHitless = eHitless;
		m_bWordDict = bWordDict;
		m_sWord[0] = '\0';
		m_iCheckpoint = 1;
		m_bHasSkips = bHasSkips;
	}

	bool Read()
	{
		if ( m_pReader->GetPos()>=m_iMaxPos )
			return false;

		// get leading value
		SphWordID_t iWord0 = m_bWordDict ? m_pReader->GetByte() : m_pReader->UnzipWordid();
		if ( !iWord0 )
		{
			// handle checkpoint
			m_iCheckpoint++;
			m_pReader->UnzipOffset();

			m_iWordID = 0;
			m_iDoclistOffset = 0;
			m_sWord[0] = '\0';

			if ( m_pReader->GetPos()>=m_iMaxPos )
				return false;

			iWord0 = m_bWordDict ? m_pReader->GetByte() : m_pReader->UnzipWordid(); // get next word
		}
		if ( !iWord0 )
			return false; // some failure

		// get word entry
		if ( m_bWordDict )
		{
			// unpack next word
			// must be in sync with DictEnd()!
			assert ( iWord0<=255 );
			BYTE uPack = (BYTE) iWord0;

			int iMatch, iDelta;
			if ( uPack & 0x80 )
			{
				iDelta = ( ( uPack>>4 ) & 7 ) + 1;
				iMatch = uPack & 15;
			} else
			{
				iDelta = uPack & 127;
				iMatch = m_pReader->GetByte();
			}
			assert ( iMatch+iDelta<(int)sizeof(m_sWord)-1 );
			assert ( iMatch<=(int)strlen(m_sWord) );

			m_pReader->GetBytes ( m_sWord + iMatch, iDelta );
			m_sWord [ iMatch+iDelta ] = '\0';

			m_iDoclistOffset = m_pReader->UnzipOffset();
			m_iDocs = m_pReader->UnzipInt();
			m_iHits = m_pReader->UnzipInt();
			m_iHint = 0;
			if ( m_iDocs>=DOCLIST_HINT_THRESH )
				m_iHint = m_pReader->GetByte();
			if ( m_bHasSkips && ( m_iDocs > SPH_SKIPLIST_BLOCK ) )
				m_pReader->UnzipInt();

			m_iWordID = (SphWordID_t) sphCRC32 ( GetWord() ); // set wordID for indexing

		} else
		{
			m_iWordID += iWord0;
			m_iDoclistOffset += m_pReader->UnzipOffset();
			m_iDocs = m_pReader->UnzipInt();
			m_iHits = m_pReader->UnzipInt();
			if ( m_bHasSkips && ( m_iDocs > SPH_SKIPLIST_BLOCK ) )
				m_pReader->UnzipOffset();
		}

		m_bHasHitlist =
			( m_eHitless==SPH_HITLESS_NONE ) ||
			( m_eHitless==SPH_HITLESS_SOME && !( m_iDocs & 0x80000000 ) );
		m_iDocs = m_eHitless==SPH_HITLESS_SOME ? ( m_iDocs & 0x7FFFFFFF ) : m_iDocs;

		return true; // FIXME? errorflag?
	}

	int CmpWord ( const CSphDictReader & tOther ) const
	{
		if ( m_bWordDict )
			return strcmp ( m_sWord, tOther.m_sWord );

		int iRes = 0;
		iRes = m_iWordID<tOther.m_iWordID ? -1 : iRes;
		iRes = m_iWordID>tOther.m_iWordID ? 1 : iRes;
		return iRes;
	}

	BYTE * GetWord() const { return (BYTE *)m_sWord; }

	int GetCheckpoint() const { return m_iCheckpoint; }
};

static ISphFilter * CreateMergeFilters ( const CSphVector<CSphFilterSettings> & dSettings,
	const CSphSchema & tSchema, const DWORD * pMvaPool, const BYTE * pStrings )
{
	CSphString sError;
	ISphFilter * pResult = NULL;
	ARRAY_FOREACH ( i, dSettings )
	{
		ISphFilter * pFilter = sphCreateFilter ( dSettings[i], tSchema, pMvaPool, pStrings, sError );
		if ( pFilter )
			pResult = sphJoinFilters ( pResult, pFilter );
	}
	return pResult;
}

static bool CheckDocsCount ( int64_t iDocs, CSphString & sError )
{
	if ( iDocs<INT_MAX )
		return true;

	sError.SetSprintf ( "index over %d documents not supported (got "INT64_FMT" documents)", INT_MAX, iDocs );
	return false;
}


class CSphMerger
{
private:
	CSphFixedVector<CSphRowitem> m_dInlineRow;
	CSphHitBuilder *	m_pHitBuilder;
	SphDocID_t			m_iMinID;

public:
	explicit CSphMerger ( CSphHitBuilder * pHitBuilder, int iInlineCount, SphDocID_t iMinID )
		: m_dInlineRow ( iInlineCount )
		, m_pHitBuilder ( pHitBuilder )
		, m_iMinID ( iMinID )
	{
	}

	template < typename QWORD > static inline
	void PrepareQword ( QWORD & tQword, const CSphDictReader & tReader, SphDocID_t iMinID, bool bWordDict ) //NOLINT
	{
		tQword.m_iMinID = iMinID;
		tQword.m_tDoc.m_iDocID = iMinID;

		tQword.m_iDocs = tReader.m_iDocs;
		tQword.m_iHits = tReader.m_iHits;
		tQword.m_bHasHitlist = tReader.m_bHasHitlist;

		tQword.m_uHitPosition = 0;
		tQword.m_iHitlistPos = 0;

		if ( bWordDict )
			tQword.m_rdDoclist.SeekTo ( tReader.m_iDoclistOffset, tReader.m_iHint );
	}

	template < typename QWORD >
	inline bool NextDocument ( QWORD & tQword, const CSphIndex_VLN * pSourceIndex, const ISphFilter * pFilter )
	{
		for ( ;; )
		{
			tQword.GetNextDoc ( m_dInlineRow.Begin() );
			if ( tQword.m_tDoc.m_iDocID )
			{
				tQword.SeekHitlist ( tQword.m_iHitlistPos );
				if ( pFilter )
				{
					CSphMatch tMatch;
					tMatch.m_iDocID = tQword.m_tDoc.m_iDocID;
					if ( pFilter->UsesAttrs() )
					{
						if ( m_dInlineRow.GetLength() )
							tMatch.m_pDynamic = m_dInlineRow.Begin();
						else
						{
							const DWORD * pInfo = pSourceIndex->FindDocinfo ( tQword.m_tDoc.m_iDocID );
							tMatch.m_pStatic = pInfo?DOCINFO2ATTRS ( pInfo ):NULL;
						}
					}
					bool bResult = pFilter->Eval ( tMatch );
					tMatch.m_pDynamic = NULL;
					if ( !bResult )
					{
						while ( tQword.m_bHasHitlist && tQword.GetNextHit()!=EMPTY_HIT );
						continue;
					}
				}
				return true;
			} else
				return false;
		}
	}

	template < typename QWORD >
	inline void TransferData ( QWORD & tQword, SphWordID_t iWordID, BYTE * sWord,
		const CSphIndex_VLN * pSourceIndex, const ISphFilter * pFilter, volatile bool * pForceTerminate )
	{
		CSphAggregateHit tHit;
		tHit.m_iWordID = iWordID;
		tHit.m_sKeyword = sWord;
		tHit.m_dFieldMask.UnsetAll();

		while ( CSphMerger::NextDocument ( tQword, pSourceIndex, pFilter ) && !*pForceTerminate )
		{
			if ( tQword.m_bHasHitlist )
				TransferHits ( tQword, tHit );
			else
			{
				// convert to aggregate if there is no hit-list
				tHit.m_iDocID = tQword.m_tDoc.m_iDocID - m_iMinID;
				tHit.m_dFieldMask = tQword.m_dQwordFields;
				tHit.SetAggrCount ( tQword.m_uMatchHits );
				m_pHitBuilder->cidxHit ( &tHit, m_dInlineRow.Begin() );
			}
		}
	}

	template < typename QWORD >
	inline void TransferHits ( QWORD & tQword, CSphAggregateHit & tHit )
	{
		assert ( tQword.m_bHasHitlist );
		tHit.m_iDocID = tQword.m_tDoc.m_iDocID - m_iMinID;
		for ( Hitpos_t uHit = tQword.GetNextHit(); uHit!=EMPTY_HIT; uHit = tQword.GetNextHit() )
		{
			tHit.m_iWordPos = uHit;
			m_pHitBuilder->cidxHit ( &tHit, m_dInlineRow.Begin() );
		}
	}

	template < typename QWORD >
	static inline void ConfigureQword ( QWORD & tQword, CSphAutofile & tHits, CSphAutofile & tDocs,
		int iDynamic, int iInline, const CSphRowitem * pMin, ThrottleState_t * pThrottle )
	{
		tQword.m_iInlineAttrs = iInline;
		tQword.m_pInlineFixup = iInline ? pMin : NULL;

		tQword.m_rdHitlist.SetThrottle ( pThrottle );
		tQword.m_rdHitlist.SetFile ( tHits );
		tQword.m_rdHitlist.GetByte();

		tQword.m_rdDoclist.SetThrottle ( pThrottle );
		tQword.m_rdDoclist.SetFile ( tDocs );
		tQword.m_rdDoclist.GetByte();

		tQword.m_tDoc.Reset ( iDynamic );
	}


	const CSphRowitem * GetInline () const { return m_dInlineRow.Begin(); }
	CSphRowitem * AcquireInline () const { return m_dInlineRow.Begin(); }
};


template < typename QWORDDST, typename QWORDSRC >
bool CSphIndex_VLN::MergeWords ( const CSphIndex_VLN * pDstIndex, const CSphIndex_VLN * pSrcIndex,
	const ISphFilter * pFilter, SphDocID_t iMinID, CSphHitBuilder * pHitBuilder, CSphString & sError,
	CSphSourceStats & tStat, CSphIndexProgress & tProgress, ThrottleState_t * pThrottle, volatile bool * pForceTerminate )
{
	CSphAutofile tDummy;
	pHitBuilder->CreateIndexFiles ( pDstIndex->GetIndexFileName("tmp.spd").cstr(),
		pDstIndex->GetIndexFileName("tmp.spp").cstr(),
		pDstIndex->GetIndexFileName("tmp.spe").cstr(),
		false, 0, tDummy, NULL );

	CSphDictReader tDstReader;
	CSphDictReader tSrcReader;

	bool bWordDict = pHitBuilder->IsWordDict();

	if ( !tDstReader.Setup ( pDstIndex->GetIndexFileName("spi"), pDstIndex->m_tWordlist.m_iWordsEnd,
		pDstIndex->m_tSettings.m_eHitless, sError, bWordDict, pThrottle, pDstIndex->m_tWordlist.m_bHaveSkips ) )
			return false;
	if ( !tSrcReader.Setup ( pSrcIndex->GetIndexFileName("spi"), pSrcIndex->m_tWordlist.m_iWordsEnd,
		pSrcIndex->m_tSettings.m_eHitless, sError, bWordDict, pThrottle, pSrcIndex->m_tWordlist.m_bHaveSkips ) )
			return false;

	const SphDocID_t iDstMinID = pDstIndex->m_iMinDocid;
	const SphDocID_t iSrcMinID = pSrcIndex->m_iMinDocid;

	/// prepare for indexing
	pHitBuilder->HitblockBegin();
	pHitBuilder->HitReset();
	pHitBuilder->SetMin ( pDstIndex->m_dMinRow.Begin(), pDstIndex->m_dMinRow.GetLength() );

	/// setup qwords

	QWORDDST tDstQword ( false, false );
	QWORDSRC tSrcQword ( false, false );

	CSphAutofile fSrcDocs, fSrcHits;
	fSrcDocs.Open ( pSrcIndex->GetIndexFileName("spd"), SPH_O_READ, sError );
	fSrcHits.Open ( pSrcIndex->GetIndexFileName("spp"), SPH_O_READ, sError );

	CSphAutofile fDstDocs, fDstHits;
	fDstDocs.Open ( pDstIndex->GetIndexFileName("spd"), SPH_O_READ, sError );
	fDstHits.Open ( pDstIndex->GetIndexFileName("spp"), SPH_O_READ, sError );

	if ( !sError.IsEmpty() || *pForceTerminate )
		return false;

	int iDstInlineSize = pDstIndex->m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE ? pDstIndex->m_tSchema.GetRowSize() : 0;
	int iSrcInlineSize = pSrcIndex->m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE ? pSrcIndex->m_tSchema.GetRowSize() : 0;

	CSphMerger tMerger ( pHitBuilder, Max ( iDstInlineSize, iSrcInlineSize ), iMinID );

	CSphMerger::ConfigureQword<QWORDDST> ( tDstQword, fDstHits, fDstDocs,
		pDstIndex->m_tSchema.GetDynamicSize(), iDstInlineSize,
		pDstIndex->m_dMinRow.Begin(), pThrottle );
	CSphMerger::ConfigureQword<QWORDSRC> ( tSrcQword, fSrcHits, fSrcDocs,
		pSrcIndex->m_tSchema.GetDynamicSize(), iSrcInlineSize,
		pSrcIndex->m_dMinRow.Begin(), pThrottle );

	/// merge

	bool bDstWord = tDstReader.Read();
	bool bSrcWord = tSrcReader.Read();

	tProgress.m_ePhase = CSphIndexProgress::PHASE_MERGE;
	tProgress.Show ( false );

	int iWords = 0;
	int iHitlistsDiscarded = 0;
	for ( ; bDstWord || bSrcWord; iWords++ )
	{
		if ( iWords==1000 )
		{
			tProgress.m_iWords += 1000;
			tProgress.Show ( false );
			iWords = 0;
		}

		if ( *pForceTerminate )
			return false;

		const int iCmp = tDstReader.CmpWord ( tSrcReader );

		if ( !bSrcWord || ( bDstWord && iCmp<0 ) )
		{
			// transfer documents and hits from destination
			CSphMerger::PrepareQword<QWORDDST> ( tDstQword, tDstReader, iDstMinID, bWordDict );
			tMerger.TransferData<QWORDDST> ( tDstQword, tDstReader.m_iWordID, tDstReader.GetWord(), pDstIndex, pFilter, pForceTerminate );
			bDstWord = tDstReader.Read();

		} else if ( !bDstWord || ( bSrcWord && iCmp>0 ) )
		{
			// transfer documents and hits from source
			CSphMerger::PrepareQword<QWORDSRC> ( tSrcQword, tSrcReader, iSrcMinID, bWordDict );
			tMerger.TransferData<QWORDSRC> ( tSrcQword, tSrcReader.m_iWordID, tSrcReader.GetWord(), pSrcIndex, NULL, pForceTerminate );
			bSrcWord = tSrcReader.Read();

		} else // merge documents and hits inside the word
		{
			assert ( iCmp==0 );

			bool bHitless = !tDstReader.m_bHasHitlist;
			if ( tDstReader.m_bHasHitlist!=tSrcReader.m_bHasHitlist )
			{
				iHitlistsDiscarded++;
				bHitless = true;
			}

			CSphMerger::PrepareQword<QWORDDST> ( tDstQword, tDstReader, iDstMinID, bWordDict );
			CSphMerger::PrepareQword<QWORDSRC> ( tSrcQword, tSrcReader, iSrcMinID, bWordDict );

			CSphAggregateHit tHit;
			tHit.m_iWordID = tDstReader.m_iWordID; // !COMMIT m_sKeyword anyone?
			tHit.m_sKeyword = tDstReader.GetWord();
			tHit.m_dFieldMask.UnsetAll();

			bool bDstDocs = tMerger.NextDocument ( tDstQword, pDstIndex, pFilter );
			bool bSrcDocs = true;

			tSrcQword.GetNextDoc ( tMerger.AcquireInline() );
			tSrcQword.SeekHitlist ( tSrcQword.m_iHitlistPos );

			while ( bDstDocs || bSrcDocs )
			{
				if ( *pForceTerminate )
					return false;

				if ( !bSrcDocs || ( bDstDocs && tDstQword.m_tDoc.m_iDocID < tSrcQword.m_tDoc.m_iDocID ) )
				{
					// transfer hits from destination
					if ( bHitless )
					{
						while ( tDstQword.m_bHasHitlist && tDstQword.GetNextHit()!=EMPTY_HIT );

						tHit.m_iDocID = tDstQword.m_tDoc.m_iDocID - iMinID;
						tHit.m_dFieldMask = tDstQword.m_dQwordFields;
						tHit.SetAggrCount ( tDstQword.m_uMatchHits );
						pHitBuilder->cidxHit ( &tHit, tMerger.GetInline() );
					} else
						tMerger.TransferHits ( tDstQword, tHit );
					bDstDocs = tMerger.NextDocument ( tDstQword, pDstIndex, pFilter );

				} else if ( !bDstDocs || ( bSrcDocs && tDstQword.m_tDoc.m_iDocID > tSrcQword.m_tDoc.m_iDocID ) )
				{
					// transfer hits from source
					if ( bHitless )
					{
						while ( tSrcQword.m_bHasHitlist && tSrcQword.GetNextHit()!=EMPTY_HIT );

						tHit.m_iDocID = tSrcQword.m_tDoc.m_iDocID - iMinID;
						tHit.m_dFieldMask = tSrcQword.m_dQwordFields;
						tHit.SetAggrCount ( tSrcQword.m_uMatchHits );
						pHitBuilder->cidxHit ( &tHit, tMerger.GetInline() );
					} else
						tMerger.TransferHits ( tSrcQword, tHit );
					bSrcDocs = tMerger.NextDocument ( tSrcQword, pSrcIndex, NULL );

				} else
				{
					// merge hits inside the document
					assert ( bDstDocs );
					assert ( bSrcDocs );
					assert ( tDstQword.m_tDoc.m_iDocID==tSrcQword.m_tDoc.m_iDocID );

					tHit.m_iDocID = tDstQword.m_tDoc.m_iDocID - iMinID;

					if ( bHitless )
					{
						while ( tDstQword.m_bHasHitlist && tDstQword.GetNextHit()!=EMPTY_HIT );
						while ( tSrcQword.m_bHasHitlist && tSrcQword.GetNextHit()!=EMPTY_HIT );

						for ( int i=0; i<FieldMask_t::SIZE; i++ )
							tHit.m_dFieldMask[i] = tDstQword.m_dQwordFields[i] | tSrcQword.m_dQwordFields[i];
						tHit.SetAggrCount ( tDstQword.m_uMatchHits + tSrcQword.m_uMatchHits );
						pHitBuilder->cidxHit ( &tHit, tMerger.GetInline() );

					} else
					{
						Hitpos_t uDstHit = tDstQword.GetNextHit();
						Hitpos_t uSrcHit = tSrcQword.GetNextHit();

						while ( uDstHit!=EMPTY_HIT || uSrcHit!=EMPTY_HIT )
						{
							if ( uSrcHit==EMPTY_HIT || ( uDstHit!=EMPTY_HIT && uDstHit<uSrcHit ) )
							{
								tHit.m_iWordPos = uDstHit;
								pHitBuilder->cidxHit ( &tHit, tMerger.GetInline() );
								uDstHit = tDstQword.GetNextHit();

							} else if ( uDstHit==EMPTY_HIT || ( uSrcHit!=EMPTY_HIT && uSrcHit<uDstHit ) )
							{
								tHit.m_iWordPos = uSrcHit;
								pHitBuilder->cidxHit ( &tHit, tMerger.GetInline() );
								uSrcHit = tSrcQword.GetNextHit();

							} else
							{
								assert ( uDstHit==uSrcHit );

								tHit.m_iWordPos = uDstHit;
								pHitBuilder->cidxHit ( &tHit, tMerger.GetInline() );

								uDstHit = tDstQword.GetNextHit();
								uSrcHit = tSrcQword.GetNextHit();
							}
						}
					}

					// next document
					bDstDocs = tMerger.NextDocument ( tDstQword, pDstIndex, pFilter );
					bSrcDocs = tMerger.NextDocument ( tSrcQword, pSrcIndex, NULL );
				}
			}
			// next word
			bDstWord = tDstReader.Read();
			bSrcWord = tSrcReader.Read();
		}
	}

	tStat.m_iTotalDocuments += pSrcIndex->m_tStats.m_iTotalDocuments;
	tStat.m_iTotalBytes += pSrcIndex->m_tStats.m_iTotalBytes;

	tProgress.m_iWords += iWords;
	tProgress.Show ( false );

	if ( iHitlistsDiscarded )
		sphWarning ( "discarded hitlists for %u words", iHitlistsDiscarded );

	return true;
}


bool CSphIndex_VLN::Merge ( CSphIndex * pSource, const CSphVector<CSphFilterSettings> & dFilters, bool bMergeKillLists )
{
	CSphString sWarning;
	if ( !Prealloc ( false, false, sWarning ) || !Preread() )
		return false;
	if ( !pSource->Prealloc ( false, false, sWarning ) || !pSource->Preread() )
	{
		m_sLastError.SetSprintf ( "source index preload failed: %s", pSource->GetLastError().cstr() );
		return false;
	}

	// create filters
	CSphScopedPtr<ISphFilter> pFilter ( CreateMergeFilters ( dFilters, m_tSchema, m_tMva.GetWritePtr(), m_tString.GetWritePtr() ) );
	DWORD nKillListSize = pSource->GetKillListSize ();
	if ( nKillListSize )
	{
		CSphFilterSettings tKillListFilter;
		const SphAttr_t * pKillList = pSource->GetKillList ();

		tKillListFilter.m_bExclude = true;
		tKillListFilter.m_eType = SPH_FILTER_VALUES;
		tKillListFilter.m_iMinValue = pKillList[0];
		tKillListFilter.m_iMaxValue = pKillList[nKillListSize -1];
		tKillListFilter.m_sAttrName = "@id";
		tKillListFilter.SetExternalValues ( pKillList, nKillListSize );

		ISphFilter * pKillListFilter = sphCreateFilter ( tKillListFilter, m_tSchema, m_tMva.GetWritePtr(), m_tString.GetWritePtr(), m_sLastError );
		pFilter = sphJoinFilters ( pFilter.LeakPtr(), pKillListFilter );
	}

	bool bForceTerminate = false;
	return CSphIndex_VLN::DoMerge ( this, (const CSphIndex_VLN *)pSource,
		bMergeKillLists, pFilter.Ptr(), m_sLastError, m_tProgress, &g_tThrottle, &bForceTerminate );
}

bool CSphIndex_VLN::DoMerge ( const CSphIndex_VLN * pDstIndex, const CSphIndex_VLN * pSrcIndex,
	bool bMergeKillLists, ISphFilter * pFilter, CSphString & sError,
	CSphIndexProgress & tProgress, ThrottleState_t * pThrottle, volatile bool * pForceTerminate )
{
	assert ( pDstIndex && pSrcIndex );

	const CSphSchema & tDstSchema = pDstIndex->m_tSchema;
	const CSphSchema & tSrcSchema = pSrcIndex->m_tSchema;
	if ( !tDstSchema.CompareTo ( tSrcSchema, sError ) )
		return false;

	if ( pDstIndex->m_tSettings.m_eHitless!=pSrcIndex->m_tSettings.m_eHitless )
	{
		sError = "hitless settings must be the same on merged indices";
		return false;
	}

	// FIXME!
	if ( pDstIndex->m_tSettings.m_eDocinfo!=pSrcIndex->m_tSettings.m_eDocinfo && !( pDstIndex->m_bIsEmpty || pSrcIndex->m_bIsEmpty ) )
	{
		sError.SetSprintf ( "docinfo storage on non-empty indexes must be the same (dst docinfo %d, empty %d, src docinfo %d, empty %d",
			pDstIndex->m_tSettings.m_eDocinfo, pDstIndex->m_bIsEmpty, pSrcIndex->m_tSettings.m_eDocinfo, pSrcIndex->m_bIsEmpty );
		return false;
	}

	if ( pDstIndex->m_pDict->GetSettings().m_bWordDict!=pSrcIndex->m_pDict->GetSettings().m_bWordDict )
	{
		sError.SetSprintf ( "dictionary types must be the same (dst dict=%s, src dict=%s )",
			pDstIndex->m_pDict->GetSettings().m_bWordDict ? "keywords" : "crc",
			pSrcIndex->m_pDict->GetSettings().m_bWordDict ? "keywords" : "crc" );
		return false;
	}

	BuildHeader_t tBuildHeader ( pDstIndex->m_tStats );

	/////////////////////////////////////////
	// merging attributes (.spa, .spm, .sps)
	/////////////////////////////////////////

	CSphWriter tSPMWriter, tSPSWriter;
	tSPMWriter.SetThrottle ( pThrottle );
	tSPSWriter.SetThrottle ( pThrottle );
	if ( !tSPMWriter.OpenFile ( pDstIndex->GetIndexFileName("tmp.spm"), sError )
		|| !tSPSWriter.OpenFile ( pDstIndex->GetIndexFileName("tmp.sps"), sError ) )
	{
		return false;
	}
	tSPSWriter.PutByte ( 0 ); // dummy byte, to reserve magic zero offset

	/// merging
	CSphVector<CSphAttrLocator> dMvaLocators;
	CSphVector<CSphAttrLocator> dStringLocators;
	for ( int i=0; i<tDstSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tInfo = tDstSchema.GetAttr(i);
		if ( tInfo.m_eAttrType==SPH_ATTR_UINT32SET )
			dMvaLocators.Add ( tInfo.m_tLocator );
		if ( tInfo.m_eAttrType==SPH_ATTR_STRING || tInfo.m_eAttrType==SPH_ATTR_JSON )
			dStringLocators.Add ( tInfo.m_tLocator );
	}
	for ( int i=0; i<tDstSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tInfo = tDstSchema.GetAttr(i);
		if ( tInfo.m_eAttrType==SPH_ATTR_INT64SET )
			dMvaLocators.Add ( tInfo.m_tLocator );
	}

	CSphVector<SphAttr_t> dPhantomKiller;

	int64_t iTotalDocuments = 0;
	bool bNeedInfinum = true;
	// minimal docid-1 for merging
	SphDocID_t iMergeInfinum = 0;

	if ( pDstIndex->m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN && pSrcIndex->m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN )
	{
		int iStride = DOCINFO_IDSIZE + pDstIndex->m_tSchema.GetRowSize();
		CSphFixedVector<CSphRowitem> dRow ( iStride );

		CSphWriter wrRows;
		wrRows.SetThrottle ( pThrottle );
		if ( !wrRows.OpenFile ( pDstIndex->GetIndexFileName("tmp.spa"), sError ) )
			return false;

		int64_t iExpectedDocs = pDstIndex->m_tStats.m_iTotalDocuments + pSrcIndex->GetStats().m_iTotalDocuments;
		AttrIndexBuilder_c tMinMax ( pDstIndex->m_tSchema );
		int64_t iMinMaxSize = tMinMax.GetExpectedSize ( iExpectedDocs );
		if ( iMinMaxSize>INT_MAX || iExpectedDocs>INT_MAX )
		{
			sError.SetSprintf ( "attribute files (.spa) over 128 GB are not supported (min-max approximate="INT64_FMT", documents count="INT64_FMT")",
				iMinMaxSize, iExpectedDocs );
			return false;
		}
		CSphFixedVector<DWORD> dMinMaxBuffer ( (int)iMinMaxSize );
		tMinMax.Prepare ( dMinMaxBuffer.Begin(), dMinMaxBuffer.Begin() + dMinMaxBuffer.GetLength() ); // FIXME!!! for over INT_MAX blocks

		const DWORD * pSrcRow = pSrcIndex->m_tAttr.GetWritePtr(); // they *can* be null if the respective index is empty
		const DWORD * pDstRow = pDstIndex->m_tAttr.GetWritePtr();

		int64_t iSrcCount = 0;
		int64_t iDstCount = 0;

		CSphMatch tMatch;
		while ( iSrcCount < pSrcIndex->m_iDocinfo || iDstCount < pDstIndex->m_iDocinfo )
		{
			if ( *pForceTerminate )
				return false;

			SphDocID_t iDstDocID, iSrcDocID;

			if ( iDstCount < pDstIndex->m_iDocinfo )
			{
				iDstDocID = DOCINFO2ID ( pDstRow );
				if ( pFilter )
				{
					tMatch.m_iDocID = iDstDocID;
					tMatch.m_pStatic = DOCINFO2ATTRS ( pDstRow );
					tMatch.m_pDynamic = NULL;
					if ( !pFilter->Eval ( tMatch ) )
					{
						pDstRow += iStride;
						iDstCount++;
						continue;
					}
				}
			} else
				iDstDocID = 0;

			if ( iSrcCount < pSrcIndex->m_iDocinfo )
				iSrcDocID = DOCINFO2ID ( pSrcRow );
			else
				iSrcDocID = 0;

			if ( ( iDstDocID && iDstDocID < iSrcDocID ) || ( iDstDocID && !iSrcDocID ) )
			{
				Verify ( tMinMax.Collect ( pDstRow, pDstIndex->m_tMva.GetWritePtr(), pDstIndex->m_tMva.GetNumEntries(), sError, true ) );

				if ( dMvaLocators.GetLength() || dStringLocators.GetLength() )
				{
					memcpy ( dRow.Begin(), pDstRow, iStride * sizeof ( CSphRowitem ) );
					CopyRowMVA ( pDstIndex->m_tMva.GetWritePtr(), dMvaLocators, iDstDocID, dRow.Begin(), tSPMWriter );
					CopyRowString ( pDstIndex->m_tString.GetWritePtr(), dStringLocators, dRow.Begin(), tSPSWriter );
					wrRows.PutBytes ( dRow.Begin(), sizeof(DWORD)*iStride );
				} else
				{
					wrRows.PutBytes ( pDstRow, sizeof(DWORD)*iStride );
				}


				tBuildHeader.m_uMinMaxIndex += iStride;
				pDstRow += iStride;
				iDstCount++;
				iTotalDocuments++;
				if ( bNeedInfinum )
				{
					bNeedInfinum = false;
					iMergeInfinum = iDstDocID - 1;
				}

			} else if ( iSrcDocID )
			{
				Verify ( tMinMax.Collect ( pSrcRow, pSrcIndex->m_tMva.GetWritePtr(), pSrcIndex->m_tMva.GetNumEntries(), sError, true ) );

				if ( dMvaLocators.GetLength() || dStringLocators.GetLength() )
				{
					memcpy ( dRow.Begin(), pSrcRow, iStride * sizeof ( CSphRowitem ) );
					CopyRowMVA ( pSrcIndex->m_tMva.GetWritePtr(), dMvaLocators, iSrcDocID, dRow.Begin(), tSPMWriter );
					CopyRowString ( pSrcIndex->m_tString.GetWritePtr(), dStringLocators, dRow.Begin(), tSPSWriter );
					wrRows.PutBytes ( dRow.Begin(), sizeof(DWORD)*iStride );
				} else
				{
					wrRows.PutBytes ( pSrcRow, sizeof(DWORD)*iStride );
				}

				tBuildHeader.m_uMinMaxIndex += iStride;
				pSrcRow += iStride;
				iSrcCount++;
				iTotalDocuments++;
				if ( bNeedInfinum )
				{
					bNeedInfinum = false;
					iMergeInfinum = iSrcDocID - 1;
				}

				if ( iDstDocID==iSrcDocID )
				{
					dPhantomKiller.Add ( iSrcDocID );
					pDstRow += iStride;
					iDstCount++;
				}
			}
		}

		if ( iTotalDocuments )
		{
			tMinMax.FinishCollect();
			iMinMaxSize = tMinMax.GetActualSize() * sizeof(DWORD);
			wrRows.PutBytes ( dMinMaxBuffer.Begin(), iMinMaxSize );
		}
		wrRows.CloseFile();
		if ( wrRows.IsError() )
			return false;

	} else if ( pDstIndex->m_bIsEmpty || pSrcIndex->m_bIsEmpty )
	{
		// one of the indexes has no documents; copy the .spa file from the other one
		CSphString sSrc = !pDstIndex->m_bIsEmpty ? pDstIndex->GetIndexFileName("spa") : pSrcIndex->GetIndexFileName("spa");
		CSphString sDst = pDstIndex->GetIndexFileName("tmp.spa");

		if ( !CopyFile ( sSrc.cstr(), sDst.cstr(), sError, pThrottle, pForceTerminate ) )
			return false;

	} else
	{
		// storage is not extern; create dummy .spa file
		CSphAutofile fdSpa ( pDstIndex->GetIndexFileName("tmp.spa"), SPH_O_NEW, sError );
		fdSpa.Close();
	}

	if ( !CheckDocsCount ( iTotalDocuments, sError ) )
		return false;

	// create phantom killlist filter
	if ( dPhantomKiller.GetLength() )
	{
		CSphFilterSettings tKLF;
		tKLF.m_bExclude = true;
		tKLF.m_eType = SPH_FILTER_VALUES;
		tKLF.m_iMinValue = dPhantomKiller[0];
		tKLF.m_iMaxValue = dPhantomKiller.Last();
		tKLF.m_sAttrName = "@id";
		tKLF.SetExternalValues ( &dPhantomKiller[0], dPhantomKiller.GetLength() );
		ISphFilter * pSpaFilter = sphCreateFilter ( tKLF, pDstIndex->m_tSchema, pDstIndex->m_tMva.GetWritePtr(), pDstIndex->m_tString.GetWritePtr(), sError );
		pFilter = sphJoinFilters ( pFilter, pSpaFilter );
	}

	CSphAutofile fdTmpDict ( pDstIndex->GetIndexFileName("tmp8.spi"), SPH_O_NEW, sError, true );
	CSphAutofile fdDict ( pDstIndex->GetIndexFileName("tmp.spi"), SPH_O_NEW, sError );

	if ( !sError.IsEmpty() || fdTmpDict.GetFD()<0 || fdDict.GetFD()<0 || *pForceTerminate )
		return false;

	CSphScopedPtr<CSphDict> pDict ( pDstIndex->m_pDict->Clone() );

	int iHitBufferSize = 8 * 1024 * 1024;
	CSphVector<SphWordID_t> dDummy;
	CSphHitBuilder tHitBuilder ( pDstIndex->m_tSettings, dDummy, true, iHitBufferSize, pDict.Ptr(), &sError );
	tHitBuilder.SetThrottle ( pThrottle );

	CSphFixedVector<CSphRowitem> dMinRow ( pDstIndex->m_dMinRow.GetLength() );
	memcpy ( dMinRow.Begin(), pDstIndex->m_dMinRow.Begin(), sizeof(CSphRowitem)*dMinRow.GetLength() );
	// correct infinum might be already set during spa merging.
	SphDocID_t iMinDocid = ( !iMergeInfinum ) ? Min ( pDstIndex->m_iMinDocid, pSrcIndex->m_iMinDocid ) : iMergeInfinum;
	tBuildHeader.m_iMinDocid = iMinDocid;
	tBuildHeader.m_pMinRow = dMinRow.Begin();

	// FIXME? is this magic dict block constant any good?..
	pDict->DictBegin ( fdTmpDict, fdDict, iHitBufferSize, pThrottle );

	// merge dictionaries, doclists and hitlists
	if ( pDict->GetSettings().m_bWordDict )
	{
		WITH_QWORD ( pDstIndex, false, QwordDst,
			WITH_QWORD ( pSrcIndex, false, QwordSrc,
		{
			if ( !CSphIndex_VLN::MergeWords < QwordDst, QwordSrc > ( pDstIndex, pSrcIndex, pFilter, iMinDocid,
					&tHitBuilder, sError, tBuildHeader, tProgress, pThrottle, pForceTerminate ) )
				return false;
		} ) );
	} else
	{
		WITH_QWORD ( pDstIndex, true, QwordDst,
			WITH_QWORD ( pSrcIndex, true, QwordSrc,
		{
			if ( !CSphIndex_VLN::MergeWords < QwordDst, QwordSrc > ( pDstIndex, pSrcIndex, pFilter, iMinDocid,
					&tHitBuilder, sError, tBuildHeader, tProgress, pThrottle, pForceTerminate ) )
				return false;
		} ) );
	}

	if ( iTotalDocuments )
		tBuildHeader.m_iTotalDocuments = iTotalDocuments;

	// merge kill-lists
	CSphAutofile fdKillList ( pDstIndex->GetIndexFileName("tmp.spk"), SPH_O_NEW, sError );
	if ( fdKillList.GetFD () < 0 )
		return false;

	if ( bMergeKillLists )
	{
		// merge spk
		CSphVector<SphAttr_t> dKillList;
		dKillList.Reserve ( pDstIndex->GetKillListSize() + pSrcIndex->GetKillListSize() );
		for ( int i = 0; i < pSrcIndex->GetKillListSize (); i++ )
			dKillList.Add ( pSrcIndex->GetKillList () [i] );

		for ( int i = 0; i < pDstIndex->GetKillListSize (); i++ )
			dKillList.Add ( pDstIndex->GetKillList () [i] );

		dKillList.Uniq ();

		tBuildHeader.m_iKillListSize = dKillList.GetLength ();

		if ( *pForceTerminate )
			return false;

		if ( dKillList.GetLength() )
		{
			if ( !sphWriteThrottled ( fdKillList.GetFD(), &dKillList[0], dKillList.GetLength()*sizeof(SphAttr_t), "kill_list", sError, pThrottle ) )
				return false;
		}
	}

	fdKillList.Close ();

	if ( *pForceTerminate )
		return false;

	// finalize
	CSphAggregateHit tFlush;
	tFlush.m_iDocID = 0;
	tFlush.m_iWordID = 0;
	tFlush.m_sKeyword = (BYTE*)""; // tricky: assertion in cidxHit calls strcmp on this in case of empty index!
	tFlush.m_iWordPos = EMPTY_HIT;
	tFlush.m_dFieldMask.UnsetAll();
	tHitBuilder.cidxHit ( &tFlush, NULL );

	if ( !tHitBuilder.cidxDone ( iHitBufferSize, pDstIndex->m_tSettings.m_iMinInfixLen,
		pDstIndex->m_pTokenizer->GetMaxCodepointLength(), &tBuildHeader ) )
			return false;

	tBuildHeader.m_sHeaderExtension = "tmp.sph";
	tBuildHeader.m_pThrottle = pThrottle;

	pDstIndex->BuildDone ( tBuildHeader, sError ); // FIXME? is this magic dict block constant any good?..

	// we're done
	tProgress.Show ( true );

	return true;
}


bool sphMerge ( const CSphIndex * pDst, const CSphIndex * pSrc, ISphFilter * pFilter,
				CSphString & sError, CSphIndexProgress & tProgress, ThrottleState_t * pThrottle, volatile bool * pForceTerminate )
{
	const CSphIndex_VLN * pDstIndex = (const CSphIndex_VLN *)pDst;
	const CSphIndex_VLN * pSrcIndex = (const CSphIndex_VLN *)pSrc;

	return CSphIndex_VLN::DoMerge ( pDstIndex, pSrcIndex, false, pFilter, sError, tProgress, pThrottle, pForceTerminate );
}


/////////////////////////////////////////////////////////////////////////////
// THE SEARCHER
/////////////////////////////////////////////////////////////////////////////

SphWordID_t CSphDictTraits::GetWordID ( BYTE * )
{
	assert ( 0 && "not implemented" );
	return 0;
}


SphWordID_t CSphDictStar::GetWordID ( BYTE * pWord )
{
	char sBuf [ 16+3*SPH_MAX_WORD_LEN ];
	assert ( strlen ( (const char*)pWord ) < 16+3*SPH_MAX_WORD_LEN );

	if ( m_pDict->GetSettings().m_bStopwordsUnstemmed && m_pDict->IsStopWord ( pWord ) )
		return 0;

	m_pDict->ApplyStemmers ( pWord );

	int iLen = strlen ( (const char*)pWord );
	assert ( iLen < 16+3*SPH_MAX_WORD_LEN - 1 );
	// stemmer might squeeze out the word
	if ( iLen && !pWord[0] )
		return 0;

	memcpy ( sBuf, pWord, iLen+1 );

	if ( iLen )
	{
		if ( sBuf[iLen-1]=='*' )
		{
			iLen--;
			sBuf[iLen] = '\0';
		} else
		{
			sBuf[iLen] = MAGIC_WORD_TAIL;
			iLen++;
			sBuf[iLen] = '\0';
		}
	}

	return m_pDict->GetWordID ( (BYTE*)sBuf, iLen, !m_pDict->GetSettings().m_bStopwordsUnstemmed );
}


SphWordID_t	CSphDictStar::GetWordIDNonStemmed ( BYTE * pWord )
{
	return m_pDict->GetWordIDNonStemmed ( pWord );
}


//////////////////////////////////////////////////////////////////////////

CSphDictStarV8::CSphDictStarV8 ( CSphDict * pDict, bool bPrefixes, bool bInfixes )
	: CSphDictStar	( pDict )
	, m_bPrefixes	( bPrefixes )
	, m_bInfixes	( bInfixes )
{
}


SphWordID_t	CSphDictStarV8::GetWordID ( BYTE * pWord )
{
	char sBuf [ 16+3*SPH_MAX_WORD_LEN ];

	int iLen = strlen ( (const char*)pWord );
	iLen = Min ( iLen, 16+3*SPH_MAX_WORD_LEN - 1 );

	if ( !iLen )
		return 0;

	bool bHeadStar = ( pWord[0]=='*' );
	bool bTailStar = ( pWord[iLen-1]=='*' ) && ( iLen>1 );

	if ( !bHeadStar && !bTailStar )
	{
		if ( m_pDict->GetSettings().m_bStopwordsUnstemmed && IsStopWord ( pWord ) )
			return 0;

		m_pDict->ApplyStemmers ( pWord );

		// stemmer might squeeze out the word
		if ( !pWord[0] )
			return 0;

		if ( !m_pDict->GetSettings().m_bStopwordsUnstemmed && IsStopWord ( pWord ) )
			return 0;
	}

	iLen = strlen ( (const char*)pWord );
	assert ( iLen < 16+3*SPH_MAX_WORD_LEN - 2 );

	if ( !iLen || ( bHeadStar && iLen==1 ) )
		return 0;

	if ( m_bInfixes )
	{
		////////////////////////////////////
		// infix or mixed infix+prefix mode
		////////////////////////////////////

		// handle head star
		if ( bHeadStar )
		{
			memcpy ( sBuf, pWord+1, iLen-- ); // chops star, copies trailing zero, updates iLen
		} else
		{
			sBuf[0] = MAGIC_WORD_HEAD;
			memcpy ( sBuf+1, pWord, ++iLen ); // copies everything incl trailing zero, updates iLen
		}

		// handle tail star
		if ( bTailStar )
		{
			sBuf[--iLen] = '\0'; // got star, just chop it away
		} else
		{
			sBuf[iLen] = MAGIC_WORD_TAIL; // no star, add tail marker
			sBuf[++iLen] = '\0';
		}

	} else
	{
		////////////////////
		// prefix-only mode
		////////////////////

		assert ( m_bPrefixes );

		// always ignore head star in prefix mode
		if ( bHeadStar )
		{
			pWord++;
			iLen--;
		}

		// handle tail star
		if ( !bTailStar )
		{
			// exact word search request, always (ie. both in infix/prefix mode) mangles to "\1word\1" in v.8+
			sBuf[0] = MAGIC_WORD_HEAD;
			memcpy ( sBuf+1, pWord, iLen );
			sBuf[iLen+1] = MAGIC_WORD_TAIL;
			sBuf[iLen+2] = '\0';
			iLen += 2;

		} else
		{
			// prefix search request, mangles to word itself (just chop away the star)
			memcpy ( sBuf, pWord, iLen );
			sBuf[--iLen] = '\0';
		}
	}

	// calc id for mangled word
	return m_pDict->GetWordID ( (BYTE*)sBuf, iLen, !bHeadStar && !bTailStar );
}

//////////////////////////////////////////////////////////////////////////

SphWordID_t CSphDictExact::GetWordID ( BYTE * pWord )
{
	int iLen = strlen ( (const char*)pWord );
	iLen = Min ( iLen, 16+3*SPH_MAX_WORD_LEN - 1 );

	if ( !iLen )
		return 0;

	if ( pWord[0]=='=' )
		pWord[0] = MAGIC_WORD_HEAD_NONSTEMMED;

	if ( pWord[0]<' ' )
		return m_pDict->GetWordIDNonStemmed ( pWord );

	return m_pDict->GetWordID ( pWord );
}


/////////////////////////////////////////////////////////////////////////////

inline bool sphGroupMatch ( SphAttr_t iGroup, const SphAttr_t * pGroups, int iGroups )
{
	if ( !pGroups ) return true;
	const SphAttr_t * pA = pGroups;
	const SphAttr_t * pB = pGroups+iGroups-1;
	if ( iGroup==*pA || iGroup==*pB ) return true;
	if ( iGroup<(*pA) || iGroup>(*pB) ) return false;

	while ( pB-pA>1 )
	{
		const SphAttr_t * pM = pA + ((pB-pA)/2);
		if ( iGroup==(*pM) )
			return true;
		if ( iGroup<(*pM) )
			pB = pM;
		else
			pA = pM;
	}
	return false;
}


bool CSphIndex_VLN::EarlyReject ( CSphQueryContext * pCtx, CSphMatch & tMatch ) const
{
	// might be needed even when we do not have a filter
	if ( pCtx->m_bLookupFilter )
		CopyDocinfo ( pCtx, tMatch, FindDocinfo ( tMatch.m_iDocID ) );
	pCtx->CalcFilter ( tMatch ); // FIXME!!! leak of filtered STRING_PTR

	return pCtx->m_pFilter ? !pCtx->m_pFilter->Eval ( tMatch ) : false;
}


SphAttr_t * CSphIndex_VLN::GetKillList () const
{
	return m_pKillList.GetWritePtr ();
}


bool CSphIndex_VLN::BuildDocList ( SphAttr_t ** ppDocList, int64_t * pCount, CSphString * pError ) const
{
	assert ( ppDocList && pCount && pError );
	*ppDocList = NULL;
	*pCount = 0;
	if ( !m_iDocinfo )
		return true;

	// new[] might fail on 32bit here
	int64_t iSizeMax = (size_t)m_iDocinfo;
	if ( iSizeMax!=m_iDocinfo )
	{
		pError->SetSprintf ( "doc-list build size_t overflow (docs count="INT64_FMT", size max="INT64_FMT")", m_iDocinfo, iSizeMax );
		return false;
	}

	int iStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
	SphAttr_t * pDst = new SphAttr_t [(size_t)m_iDocinfo];
	*ppDocList = pDst;
	*pCount = m_iDocinfo;

	const CSphRowitem * pRow = m_tAttr.GetWritePtr();
	const CSphRowitem * pEnd = m_tAttr.GetWritePtr() + m_iDocinfo*iStride;
	while ( pRow<pEnd )
	{
		*pDst++ = DOCINFO2ID ( pRow );
		pRow += iStride;
	}

	return true;
}

bool CSphIndex_VLN::ReplaceKillist ( const SphAttr_t * pKillist, int iCount )
{
	// dump killlist
	CSphAutofile fdKillList ( GetIndexFileName("spk"), SPH_O_NEW, m_sLastError );
	if ( fdKillList.GetFD()<0 )
		return false;

	if ( !sphWriteThrottled ( fdKillList.GetFD(), pKillist, iCount*sizeof(SphAttr_t), "kill list", m_sLastError, &g_tThrottle ) )
		return false;

	fdKillList.Close ();

	BuildHeader_t tBuildHeader ( m_tStats );
	(DictHeader_t &)tBuildHeader = (DictHeader_t)m_tWordlist;
	tBuildHeader.m_sHeaderExtension = "sph";
	tBuildHeader.m_pThrottle = &g_tThrottle;
	tBuildHeader.m_iMinDocid = m_iMinDocid;
	tBuildHeader.m_iKillListSize = iCount;
	tBuildHeader.m_uMinMaxIndex = m_uMinMaxIndex;

	if ( !BuildDone ( tBuildHeader, m_sLastError ) )
		return false;

	m_pKillList.Reset ();
	m_iKillListSize = 0;
	if ( iCount )
	{
		if ( !m_pKillList.Alloc ( iCount, m_sLastError, m_sLastWarning ) )
			return false;

		memcpy ( m_pKillList.GetWritePtr(), pKillist, sizeof(SphAttr_t)*iCount );
		m_iKillListSize = iCount;
	}

	return true;
}


bool CSphIndex_VLN::HasDocid ( SphDocID_t uDocid ) const
{
	return FindDocinfo ( uDocid )!=NULL;
}


const DWORD * CSphIndex_VLN::FindDocinfo ( SphDocID_t uDocID ) const
{
	if ( m_iDocinfo<=0 )
		return NULL;

	assert ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN );
	assert ( !m_tAttr.IsEmpty() );
	assert ( m_tSchema.GetAttrsCount() );

	int iStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
	int64_t iStart = 0;
	int64_t iEnd = m_iDocinfo-1;

#define LOC_ROW(_index) &m_tAttr [ _index*iStride ]
#define LOC_ID(_index) DOCINFO2ID(LOC_ROW(_index))

	if ( m_pDocinfoHash.GetLengthBytes() )
	{
		SphDocID_t uFirst = LOC_ID(0);
		SphDocID_t uLast = LOC_ID(iEnd);
		if ( uDocID<uFirst || uDocID>uLast )
			return NULL;

		int64_t iHash = ( ( uDocID - uFirst ) >> m_pDocinfoHash[0] );
		if ( iHash > ( 1 << DOCINFO_HASH_BITS ) ) // possible in case of broken data, for instance
			return NULL;

		iStart = m_pDocinfoHash [ iHash+1 ];
		iEnd = m_pDocinfoHash [ iHash+2 ] - 1;
	}

	if ( uDocID==LOC_ID(iStart) )
		return LOC_ROW(iStart);

	if ( uDocID==LOC_ID(iEnd) )
		return LOC_ROW(iEnd);

	while ( iEnd-iStart>1 )
	{
		// check if nothing found
		if ( uDocID<LOC_ID(iStart) || uDocID>LOC_ID(iEnd) )
			return NULL;
		assert ( uDocID > LOC_ID(iStart) );
		assert ( uDocID < LOC_ID(iEnd) );

		int64_t iMid = iStart + (iEnd-iStart)/2;
		if ( uDocID==LOC_ID(iMid) )
			return LOC_ROW(iMid);
		else if ( uDocID<LOC_ID(iMid) )
			iEnd = iMid;
		else
			iStart = iMid;
	}

#undef LOC_ID
#undef LOC_ROW

	return NULL;
}

void CSphIndex_VLN::CopyDocinfo ( const CSphQueryContext * pCtx, CSphMatch & tMatch, const DWORD * pFound ) const
{
	if ( !pFound )
		return;

	// setup static pointer
	assert ( DOCINFO2ID(pFound)==tMatch.m_iDocID );
	tMatch.m_pStatic = DOCINFO2ATTRS(pFound);

	// patch if necessary
	if ( pCtx->m_pOverrides )
		ARRAY_FOREACH ( i, (*pCtx->m_pOverrides) )
	{
		const CSphAttrOverride & tOverride = (*pCtx->m_pOverrides)[i]; // shortcut
		const CSphAttrOverride::IdValuePair_t * pEntry = tOverride.m_dValues.BinarySearch (
			bind ( &CSphAttrOverride::IdValuePair_t::m_uDocID ), tMatch.m_iDocID );
		tMatch.SetAttr ( pCtx->m_dOverrideOut[i], pEntry
			? pEntry->m_uValue
			: sphGetRowAttr ( tMatch.m_pStatic, pCtx->m_dOverrideIn[i] ) );
	}
}


static inline void CalcContextItems ( CSphMatch & tMatch, const CSphVector<CSphQueryContext::CalcItem_t> & dItems )
{
	ARRAY_FOREACH ( i, dItems )
	{
		const CSphQueryContext::CalcItem_t & tCalc = dItems[i];
		if ( tCalc.m_eType==SPH_ATTR_INTEGER )
			tMatch.SetAttr ( tCalc.m_tLoc, tCalc.m_pExpr->IntEval(tMatch) );
		else if ( tCalc.m_eType==SPH_ATTR_BIGINT || tCalc.m_eType==SPH_ATTR_JSON_FIELD )
			tMatch.SetAttr ( tCalc.m_tLoc, tCalc.m_pExpr->Int64Eval(tMatch) );
		else if ( tCalc.m_eType==SPH_ATTR_STRINGPTR )
		{
			const BYTE * pStr = NULL;
			tCalc.m_pExpr->StringEval ( tMatch, &pStr );
			tMatch.SetAttr ( tCalc.m_tLoc, (SphAttr_t) pStr ); // FIXME! a potential leak of *previous* value?
		} else if ( tCalc.m_eType==SPH_ATTR_FACTORS )
			tMatch.SetAttr ( tCalc.m_tLoc, (SphAttr_t)tCalc.m_pExpr->FactorEval(tMatch) );
		else
			tMatch.SetAttrFloat ( tCalc.m_tLoc, tCalc.m_pExpr->Eval(tMatch) );
	}
}

void CSphQueryContext::CalcFilter ( CSphMatch & tMatch ) const
{
	CalcContextItems ( tMatch, m_dCalcFilter );
}


void CSphQueryContext::CalcSort ( CSphMatch & tMatch ) const
{
	CalcContextItems ( tMatch, m_dCalcSort );
}


void CSphQueryContext::CalcFinal ( CSphMatch & tMatch ) const
{
	CalcContextItems ( tMatch, m_dCalcFinal );
}

static inline void FreeStrItems ( CSphMatch & tMatch, const CSphVector<CSphQueryContext::CalcItem_t> & dItems )
{
	if ( !tMatch.m_pDynamic )
		return;

	ARRAY_FOREACH ( i, dItems )
	{
		const CSphQueryContext::CalcItem_t & tCalc = dItems[i];
		switch ( tCalc.m_eType )
		{
		case SPH_ATTR_STRINGPTR:
			{
				CSphString sStr;
				sStr.Adopt ( (char**) (tMatch.m_pDynamic+tCalc.m_tLoc.m_iBitOffset/ROWITEM_BITS));
			}
			break;

		case SPH_ATTR_FACTORS:
			{
				BYTE * pData = (BYTE *)tMatch.GetAttr ( tCalc.m_tLoc );
				delete [] pData;
				tMatch.SetAttr ( tCalc.m_tLoc, 0 );
			}
			break;
		default:
			break;
		}
	}
}

void CSphQueryContext::FreeStrFilter ( CSphMatch & tMatch ) const
{
	FreeStrItems ( tMatch, m_dCalcFilter );
}


void CSphQueryContext::FreeStrSort ( CSphMatch & tMatch ) const
{
	FreeStrItems ( tMatch, m_dCalcSort );
}


void CSphQueryContext::FreeStrFinal ( CSphMatch & tMatch ) const
{
	FreeStrItems ( tMatch, m_dCalcFinal );
}


void CSphQueryContext::ExprCommand ( ESphExprCommand eCmd, void * pArg )
{
	ARRAY_FOREACH ( i, m_dCalcFilter )
		m_dCalcFilter[i].m_pExpr->Command ( eCmd, pArg );
	ARRAY_FOREACH ( i, m_dCalcSort )
		m_dCalcSort[i].m_pExpr->Command ( eCmd, pArg );
	ARRAY_FOREACH ( i, m_dCalcFinal )
		m_dCalcFinal[i].m_pExpr->Command ( eCmd, pArg );
}


void CSphQueryContext::SetStringPool ( const BYTE * pStrings )
{
	ExprCommand ( SPH_EXPR_SET_STRING_POOL, (void*)pStrings );
	if ( m_pFilter )
		m_pFilter->SetStringStorage ( pStrings );
	if ( m_pWeightFilter )
		m_pWeightFilter->SetStringStorage ( pStrings );
}


void CSphQueryContext::SetMVAPool ( const DWORD * pMva )
{
	ExprCommand ( SPH_EXPR_SET_MVA_POOL, (void*)pMva );
	if ( m_pFilter )
		m_pFilter->SetMVAStorage ( pMva );
	if ( m_pWeightFilter )
		m_pWeightFilter->SetMVAStorage ( pMva );
}


/// FIXME, perhaps
/// this rather crappy helper class really serves exactly 1 (one) simple purpose
///
/// it passes a sorting queue internals (namely, weight and float sortkey, if any,
/// of the current-worst queue element) to the MIN_TOP_WORST() and MIN_TOP_SORTVAL()
/// expression classes that expose those to the cruel outside world
///
/// all the COM-like EXTRA_xxx message back and forth is needed because expressions
/// are currently parsed and created earlier than the sorting queue
///
/// that also is the reason why we mischievously return 0 instead of clearly failing
/// with an error when the sortval is not a dynamic float; by the time we are parsing
/// expressions, we do not *yet* know that; but by the time we create a sorting queue,
/// we do not *want* to leak select expression checks into it
///
/// alternatively, we probably want to refactor this and introduce Bind(), to parse
/// expressions once, then bind them to actual searching contexts (aka index or segment,
/// and ranker, and sorter, and whatever else might be referenced by the expressions)
struct ContextExtra : public ISphExtra
{
	ISphRanker * m_pRanker;
	ISphMatchSorter * m_pSorter;

	virtual bool ExtraDataImpl ( ExtraData_e eData, void ** ppArg )
	{
		if ( eData==EXTRA_GET_QUEUE_WORST || eData==EXTRA_GET_QUEUE_SORTVAL )
		{
			if ( !m_pSorter )
				return false;
			const CSphMatch * pWorst = m_pSorter->GetWorst();
			if ( !pWorst )
				return false;
			if ( eData==EXTRA_GET_QUEUE_WORST )
			{
				*ppArg = (void*)pWorst;
				return true;
			} else
			{
				assert ( eData==EXTRA_GET_QUEUE_SORTVAL );
				const CSphMatchComparatorState & tCmp = m_pSorter->GetState();
				if ( tCmp.m_eKeypart[0]==SPH_KEYPART_FLOAT && tCmp.m_tLocator[0].m_bDynamic
					&& tCmp.m_tLocator[0].m_iBitCount==32 && ( tCmp.m_tLocator[0].m_iBitOffset%32==0 )
					&& tCmp.m_eKeypart[1]==SPH_KEYPART_ID && tCmp.m_dAttrs[1]==-1 )
				{
					*(int*)ppArg = tCmp.m_tLocator[0].m_iBitOffset/32;
					return true;
				} else
				{
					// min_top_sortval() only works with order by float_expr for now
					return false;
				}
			}
		}
		return m_pRanker->ExtraData ( eData, ppArg );
	}
};


void CSphQueryContext::SetupExtraData ( ISphRanker * pRanker, ISphMatchSorter * pSorter )
{
	ContextExtra tExtra;
	tExtra.m_pRanker = pRanker;
	tExtra.m_pSorter = pSorter;
	ExprCommand ( SPH_EXPR_SET_EXTRA_DATA, &tExtra );
}


void CSphIndex_VLN::MatchExtended ( CSphQueryContext * pCtx, const CSphQuery * pQuery,
	int iSorters, ISphMatchSorter ** ppSorters, ISphRanker * pRanker, int iTag, int iIndexWeight ) const
{
	CSphQueryProfile * pProfile = pCtx->m_pProfile;

	int iCutoff = pQuery->m_iCutoff;
	if ( iCutoff<=0 )
		iCutoff = -1;

	// do searching
	CSphMatch * pMatch = pRanker->GetMatchesBuffer();
	for ( ;; )
	{
		// ranker does profile switches internally
		int iMatches = pRanker->GetMatches();
		if ( iMatches<=0 )
			break;

		if ( pProfile )
			pProfile->Switch ( SPH_QSTATE_SORT );
		for ( int i=0; i<iMatches; i++ )
		{
			if ( pCtx->m_bLookupSort )
				CopyDocinfo ( pCtx, pMatch[i], FindDocinfo ( pMatch[i].m_iDocID ) );

			pMatch[i].m_iWeight *= iIndexWeight;
			pCtx->CalcSort ( pMatch[i] );

			if ( pCtx->m_pWeightFilter && !pCtx->m_pWeightFilter->Eval ( pMatch[i] ) )
			{
				pCtx->FreeStrSort ( pMatch[i] );
				continue;
			}

			pMatch[i].m_iTag = iTag;

			bool bRand = false;
			bool bNewMatch = false;
			for ( int iSorter=0; iSorter<iSorters; iSorter++ )
			{
				// all non-random sorters are in the beginning,
				// so we can avoid the simple 'first-element' assertion
				if ( !bRand && ppSorters[iSorter]->m_bRandomize )
				{
					bRand = true;
					pMatch[i].m_iWeight = ( sphRand() & 0xffff ) * iIndexWeight;

					if ( pCtx->m_pWeightFilter && !pCtx->m_pWeightFilter->Eval ( pMatch[i] ) )
						break;
				}
				bNewMatch |= ppSorters[iSorter]->Push ( pMatch[i] );

				if ( pCtx->m_bPackedFactors )
				{
					pRanker->ExtraData ( EXTRA_SET_MATCHPUSHED, (void**)&(ppSorters[iSorter]->m_iJustPushed) );
					pRanker->ExtraData ( EXTRA_SET_MATCHPOPPED, (void**)&(ppSorters[iSorter]->m_dJustPopped) );
				}
			}
			pCtx->FreeStrSort ( pMatch[i] );

			if ( bNewMatch )
				if ( --iCutoff==0 )
					break;
		}

		if ( iCutoff==0 )
			break;
	}

	if ( pProfile )
		pProfile->Switch ( SPH_QSTATE_UNKNOWN );
}

//////////////////////////////////////////////////////////////////////////


struct SphFinalMatchCalc_t : ISphMatchProcessor, ISphNoncopyable
{
	const CSphIndex_VLN *		m_pDocinfoSrc;
	const CSphQueryContext &	m_tCtx;
	int							m_iTag;

	SphFinalMatchCalc_t ( int iTag, const CSphIndex_VLN * pIndex, const CSphQueryContext & tCtx )
		: m_pDocinfoSrc ( pIndex )
		, m_tCtx ( tCtx )
		, m_iTag ( iTag )
	{ }

	virtual void Process ( CSphMatch * pMatch )
	{
		if ( pMatch->m_iTag>=0 )
			return;

		if ( m_pDocinfoSrc )
			m_pDocinfoSrc->CopyDocinfo ( &m_tCtx, *pMatch, m_pDocinfoSrc->FindDocinfo ( pMatch->m_iDocID ) );

		m_tCtx.CalcFinal ( *pMatch );
		pMatch->m_iTag = m_iTag;
	}
};


bool CSphIndex_VLN::MultiScan ( const CSphQuery * pQuery, CSphQueryResult * pResult,
	int iSorters, ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const
{
	assert ( pQuery->m_sQuery.IsEmpty() );
	assert ( tArgs.m_iTag>=0 );

	// check if index is ready
	if ( !m_pPreread || !*m_pPreread )
	{
		pResult->m_sError = "index not preread";
		return false;
	}

	// check if index supports scans
	if ( m_tSettings.m_eDocinfo!=SPH_DOCINFO_EXTERN || !m_tSchema.GetAttrsCount() )
	{
		pResult->m_sError = "fullscan requires extern docinfo";
		return false;
	}

	// we count documents only (before filters)
	if ( pQuery->m_iMaxPredictedMsec )
		pResult->m_bHasPrediction = true;

	if ( tArgs.m_bFactors )
		pResult->m_sWarning.SetSprintf ( "packedfactors() will not work with a fullscan; you need to specify a query" );

	// check if index has data
	if ( m_bIsEmpty || m_iDocinfo<=0 || m_tAttr.IsEmpty() )
		return true;

	// start counting
	int64_t tmQueryStart = sphMicroTimer();

	// select the sorter with max schema
	int iMaxSchemaSize = -1;
	int iMaxSchemaIndex = -1;
	for ( int i=0; i<iSorters; i++ )
		if ( ppSorters[i]->GetSchema().GetRowSize() > iMaxSchemaSize )
		{
			iMaxSchemaSize = ppSorters[i]->GetSchema().GetRowSize();
			iMaxSchemaIndex = i;
		}

	// setup calculations and result schema
	CSphQueryContext tCtx;
	if ( !tCtx.SetupCalc ( pResult, ppSorters[iMaxSchemaIndex]->GetSchema(), m_tSchema, m_tMva.GetWritePtr() ) )
		return false;

	// set string pool for string on_sort expression fix up
	tCtx.SetStringPool ( m_tString.GetWritePtr() );

	// setup filters
	if ( !tCtx.CreateFilters ( true, &pQuery->m_dFilters, ppSorters[iMaxSchemaIndex]->GetSchema(), m_tMva.GetWritePtr(), m_tString.GetWritePtr(), pResult->m_sError ) )
		return false;
	if ( !tCtx.CreateFilters ( true, tArgs.m_pExtraFilters, ppSorters[iMaxSchemaIndex]->GetSchema(), m_tMva.GetWritePtr(), m_tString.GetWritePtr(), pResult->m_sError ) )
		return false;

	// check if we can early reject the whole index
	if ( tCtx.m_pFilter && m_iDocinfoIndex )
	{
		DWORD uStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
		DWORD * pMinEntry = const_cast<DWORD*> ( &m_pDocinfoIndex [ m_iDocinfoIndex*uStride*2 ] );
		DWORD * pMaxEntry = pMinEntry + uStride;

		if ( !tCtx.m_pFilter->EvalBlock ( pMinEntry, pMaxEntry ) )
		{
			pResult->m_iQueryTime += (int)( ( sphMicroTimer()-tmQueryStart )/1000 );
			return true;
		}
	}

	// setup lookup
	tCtx.m_bLookupFilter = false;
	tCtx.m_bLookupSort = true;

	// setup sorters vs. MVA
	for ( int i=0; i<iSorters; i++ )
	{
		(ppSorters[i])->SetMVAPool ( m_tMva.GetWritePtr() );
		(ppSorters[i])->SetStringPool ( m_tString.GetWritePtr() );
	}

	// setup overrides
	if ( !tCtx.SetupOverrides ( pQuery, pResult, m_tSchema, ppSorters[iMaxSchemaIndex]->GetSchema() ) )
		return false;

	// prepare to work them rows
	bool bRandomize = ppSorters[0]->m_bRandomize;

	CSphMatch tMatch;
	tMatch.Reset ( ppSorters[iMaxSchemaIndex]->GetSchema().GetDynamicSize() );
	tMatch.m_iWeight = tArgs.m_iIndexWeight;
	tMatch.m_iTag = tCtx.m_dCalcFinal.GetLength() ? -1 : tArgs.m_iTag;

	// optimize direct lookups by id
	// run full scan with block and row filtering for everything else
	if ( pQuery->m_dFilters.GetLength()==1
		&& pQuery->m_dFilters[0].m_eType==SPH_FILTER_VALUES
		&& pQuery->m_dFilters[0].m_bExclude==false
		&& pQuery->m_dFilters[0].m_sAttrName=="@id"
		&& !tArgs.m_pExtraFilters )
	{
		// run id lookups
		for ( int i=0; i<pQuery->m_dFilters[0].GetNumValues(); i++ )
		{
			SphDocID_t uDocid = (SphDocID_t) pQuery->m_dFilters[0].GetValue(i);
			const DWORD * pRow = FindDocinfo ( uDocid );

			pResult->m_tStats.m_iFetchedDocs++;

			if ( !pRow )
				continue;

			assert ( uDocid==DOCINFO2ID(pRow) );
			tMatch.m_iDocID = uDocid;
			CopyDocinfo ( &tCtx, tMatch, pRow );

			if ( bRandomize )
				tMatch.m_iWeight = ( sphRand() & 0xffff ) * tArgs.m_iIndexWeight;

			// submit match to sorters
			tCtx.CalcSort ( tMatch );

			for ( int iSorter=0; iSorter<iSorters; iSorter++ )
				ppSorters[iSorter]->Push ( tMatch );

			// stringptr expressions should be duplicated (or taken over) at this point
			tCtx.FreeStrSort ( tMatch );
		}
	} else
	{
		// do scan
		DWORD uStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
		int64_t iStart = 0;
		int64_t iEnd = m_iDocinfoIndex;
		int iStep = 1;
		if ( pQuery->m_bReverseScan )
		{
			iStart = m_iDocinfoIndex-1;
			iEnd = -1;
			iStep = -1;
		}

		int iCutoff = pQuery->m_iCutoff;
		if ( iCutoff<=0 )
			iCutoff = -1;

		for ( int64_t iIndexEntry=iStart; iIndexEntry!=iEnd; iIndexEntry+=iStep )
		{
			// block-level filtering
			const DWORD * pMin = &m_pDocinfoIndex[ iIndexEntry*uStride*2 ];
			const DWORD * pMax = pMin + uStride;

			// check applicable filters
			if ( tCtx.m_pFilter && !tCtx.m_pFilter->EvalBlock ( pMin, pMax ) )
				continue;

			// row-level filtering
			const DWORD * pBlockStart = m_tAttr.GetWritePtr() + ( iIndexEntry*uStride*DOCINFO_INDEX_FREQ );
			const DWORD * pBlockEnd = m_tAttr.GetWritePtr() + ( ( Min ( ( iIndexEntry+1 )*DOCINFO_INDEX_FREQ, m_iDocinfo ) - 1 ) * uStride );

			if ( !tCtx.m_pOverrides && tCtx.m_pFilter && !pQuery->m_iCutoff
				&& !tCtx.m_dCalcFilter.GetLength() && !tCtx.m_dCalcSort.GetLength() )
			{
				// kinda fastpath
				for ( const DWORD * pDocinfo=pBlockStart; pDocinfo<=pBlockEnd; pDocinfo+=uStride )
				{
					tMatch.m_iDocID = DOCINFO2ID ( pDocinfo );
					tMatch.m_pStatic = DOCINFO2ATTRS ( pDocinfo );

					pResult->m_tStats.m_iFetchedDocs++;

					if ( !tCtx.m_pFilter->Eval ( tMatch ) )
					{
						tCtx.FreeStrFilter ( tMatch );
						continue;
					}

					if ( bRandomize )
						tMatch.m_iWeight = ( sphRand() & 0xffff ) * tArgs.m_iIndexWeight;
					for ( int iSorter=0; iSorter<iSorters; iSorter++ )
						ppSorters[iSorter]->Push ( tMatch );

					// stringptr expressions should be duplicated (or taken over) at this point
					tCtx.FreeStrFilter ( tMatch );
				}
			} else
			{
				// generic path
				for ( const DWORD * pDocinfo=pBlockStart; pDocinfo<=pBlockEnd; pDocinfo+=uStride )
				{
					tMatch.m_iDocID = DOCINFO2ID ( pDocinfo );
					if ( !tCtx.m_pOverrides )
						tMatch.m_pStatic = DOCINFO2ATTRS ( pDocinfo );
					else
						CopyDocinfo ( &tCtx, tMatch, pDocinfo );

					pResult->m_tStats.m_iFetchedDocs++;

					// early filter only (no late filters in full-scan because of no @weight)
					tCtx.CalcFilter ( tMatch );
					if ( tCtx.m_pFilter && !tCtx.m_pFilter->Eval ( tMatch ) )
					{
						tCtx.FreeStrFilter ( tMatch );
						continue;
					}

					if ( bRandomize )
						tMatch.m_iWeight = ( sphRand() & 0xffff ) * tArgs.m_iIndexWeight;

					// submit match to sorters
					tCtx.CalcSort ( tMatch );

					bool bNewMatch = false;
					for ( int iSorter=0; iSorter<iSorters; iSorter++ )
						bNewMatch |= ppSorters[iSorter]->Push ( tMatch );

					// stringptr expressions should be duplicated (or taken over) at this point
					tCtx.FreeStrFilter ( tMatch );
					tCtx.FreeStrSort ( tMatch );

					// handle cutoff
					if ( bNewMatch && --iCutoff==0 )
					{
						iIndexEntry = iEnd - iStep; // outer break
						break;
					}
				}
			}
		}
	}

	// do final expression calculations
	if ( tCtx.m_dCalcFinal.GetLength() )
	{
		SphFinalMatchCalc_t tFinal ( tArgs.m_iTag, NULL, tCtx );
		for ( int iSorter=0; iSorter<iSorters; iSorter++ )
		{
			ISphMatchSorter * pTop = ppSorters[iSorter];
			pTop->Finalize ( tFinal, false );
		}
	}

	// done
	pResult->m_pMva = m_tMva.GetWritePtr();
	pResult->m_pStrings = m_tString.GetWritePtr();
	pResult->m_iQueryTime += (int)( ( sphMicroTimer()-tmQueryStart )/1000 );
	return true;
}

//////////////////////////////////////////////////////////////////////////////

ISphQword * DiskIndexQwordSetup_c::QwordSpawn ( const XQKeyword_t & tWord ) const
{
	WITH_QWORD ( m_pIndex, false, Qword, return new Qword ( tWord.m_bExpanded, tWord.m_bExcluded ) );
	return NULL;
}


bool DiskIndexQwordSetup_c::QwordSetup ( ISphQword * pWord ) const
{
	WITH_QWORD ( m_pIndex, false, Qword, return Setup ( pWord ) );
	return false;
}


bool DiskIndexQwordSetup_c::Setup ( ISphQword * pWord ) const
{
	// there was a dynamic_cast here once but it's not necessary
	// maybe it worth to rewrite class hierarchy to avoid c-cast here?
	DiskIndexQwordTraits_c & tWord = *(DiskIndexQwordTraits_c*)pWord;

	// setup attrs
	tWord.m_tDoc.Reset ( m_iDynamicRowitems );
	tWord.m_iMinID = m_iMinDocid;
	tWord.m_tDoc.m_iDocID = m_iMinDocid;

	if ( m_eDocinfo==SPH_DOCINFO_INLINE )
	{
		tWord.m_iInlineAttrs = m_iInlineRowitems;
		tWord.m_pInlineFixup = m_pMinRow;
	} else
	{
		tWord.m_iInlineAttrs = 0;
		tWord.m_pInlineFixup = NULL;
	}

	// setup stats
	tWord.m_iDocs = 0;
	tWord.m_iHits = 0;

	CSphIndex_VLN * pIndex = (CSphIndex_VLN *)m_pIndex;

	// !COMMIT FIXME!
	// the below stuff really belongs in wordlist
	// which in turn really belongs in dictreader
	// which in turn might or might not be a part of dict

	// binary search through checkpoints for a one whose range matches word ID
	assert ( pIndex->m_pPreread && *pIndex->m_pPreread );
	assert ( !pIndex->m_bPreloadWordlist || !pIndex->m_tWordlist.m_pBuf.IsEmpty() );

	// empty index?
	if ( !pIndex->m_tWordlist.m_dCheckpoints.GetLength() )
		return false;

	const char * sWord = tWord.m_sDictWord.cstr();
	const bool bWordDict = pIndex->m_pDict->GetSettings().m_bWordDict;
	int iWordLen = sWord ? strlen ( sWord ) : 0;
	if ( pIndex->m_bEnableStar && bWordDict && tWord.m_sWord.Ends("*") )
	{
		iWordLen = Max ( iWordLen-1, 0 );

		// might match either infix or prefix
		int iMinLen = Max ( pIndex->m_tSettings.m_iMinPrefixLen, pIndex->m_tSettings.m_iMinInfixLen );
		if ( pIndex->m_tSettings.m_iMinPrefixLen )
			iMinLen = Min ( iMinLen, pIndex->m_tSettings.m_iMinPrefixLen );
		if ( pIndex->m_tSettings.m_iMinInfixLen )
			iMinLen = Min ( iMinLen, pIndex->m_tSettings.m_iMinInfixLen );

		// bail out term shorter than prefix or infix allowed
		if ( iWordLen<iMinLen )
			return false;
	}

	// leading special symbols trimming
	if ( pIndex->m_bEnableStar && bWordDict && tWord.m_sDictWord.Begins("*") )
	{
		sWord++;
		iWordLen = Max ( iWordLen-1, 0 );
		// bail out term shorter than infix allowed
		if ( iWordLen<pIndex->m_tSettings.m_iMinInfixLen )
			return false;
	}

	const CSphWordlistCheckpoint * pCheckpoint = pIndex->m_tWordlist.FindCheckpoint ( sWord, iWordLen, tWord.m_iWordID, false );
	if ( !pCheckpoint )
		return false;

	// decode wordlist chunk
	const BYTE * pBuf = pIndex->m_tWordlist.AcquireDict ( pCheckpoint, m_tWordlist.GetFD(), m_pDictBuf );
	assert ( pBuf );

	CSphDictEntry tRes;
	if ( bWordDict )
	{
		KeywordsBlockReader_c tCtx ( pBuf, m_pSkips!=NULL );
		while ( tCtx.UnpackWord() )
		{
			// block is sorted
			// so once keywords are greater than the reference word, no more matches
			assert ( tCtx.GetWordLen()>0 );
			int iCmp = sphDictCmpStrictly ( sWord, iWordLen, tCtx.GetWord(), tCtx.GetWordLen() );
			if ( iCmp<0 )
				return false;
			if ( iCmp==0 )
				break;
		}
		if ( tCtx.GetWordLen()<=0 )
			return false;
		tRes = tCtx;

	} else
	{
		if ( !pIndex->m_tWordlist.GetWord ( pBuf, tWord.m_iWordID, tRes ) )
			return false;
	}

	const ESphHitless eMode = pIndex->m_tSettings.m_eHitless;
	tWord.m_iDocs = eMode==SPH_HITLESS_SOME ? ( tRes.m_iDocs & 0x7FFFFFFF ) : tRes.m_iDocs;
	tWord.m_iHits = tRes.m_iHits;
	tWord.m_bHasHitlist =
		( eMode==SPH_HITLESS_NONE ) ||
		( eMode==SPH_HITLESS_SOME && !( tRes.m_iDocs & 0x80000000 ) );

	if ( m_bSetupReaders )
	{
		tWord.m_rdDoclist.SetBuffers ( g_iReadBuffer, g_iReadUnhinted );
		tWord.m_rdDoclist.SetFile ( m_tDoclist );
		tWord.m_rdDoclist.m_pProfile = m_pProfile;
		tWord.m_rdDoclist.m_eProfileState = SPH_QSTATE_READ_DOCS;

		// read in skiplist
		// OPTIMIZE? maybe cache hot decompressed lists?
		// OPTIMIZE? maybe add an option to decompress on preload instead?
		if ( m_pSkips && tRes.m_iDocs>SPH_SKIPLIST_BLOCK )
		{
			const BYTE * pSkip = m_pSkips + tRes.m_iSkiplistOffset;

			tWord.m_dSkiplist.Add();
			tWord.m_dSkiplist.Last().m_iBaseDocid = 0;
			tWord.m_dSkiplist.Last().m_iOffset = tRes.m_iDoclistOffset;
			tWord.m_dSkiplist.Last().m_iBaseHitlistPos = 0;

			for ( int i=1; i<( tWord.m_iDocs/SPH_SKIPLIST_BLOCK ); i++ )
			{
				SkiplistEntry_t & t = tWord.m_dSkiplist.Add();
				SkiplistEntry_t & p = tWord.m_dSkiplist [ tWord.m_dSkiplist.GetLength()-2 ];
				t.m_iBaseDocid = p.m_iBaseDocid + SPH_SKIPLIST_BLOCK + (SphDocID_t) sphUnzipOffset ( pSkip );
				t.m_iOffset = p.m_iOffset + 4*SPH_SKIPLIST_BLOCK + sphUnzipOffset ( pSkip );
				t.m_iBaseHitlistPos = p.m_iBaseHitlistPos + sphUnzipOffset ( pSkip );
			}
		}

		tWord.m_rdDoclist.SeekTo ( tRes.m_iDoclistOffset, tRes.m_iDoclistHint );

		tWord.m_rdHitlist.SetBuffers ( g_iReadBuffer, g_iReadUnhinted );
		tWord.m_rdHitlist.SetFile ( m_tHitlist );
		tWord.m_rdHitlist.m_pProfile = m_pProfile;
		tWord.m_rdHitlist.m_eProfileState = SPH_QSTATE_READ_HITS;
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////////

bool CSphIndex_VLN::Lock ()
{
	CSphString sName = GetIndexFileName("spl");
	sphLogDebug ( "Locking the index via file %s", sName.cstr() );

	if ( m_iLockFD<0 )
	{
		m_iLockFD = ::open ( sName.cstr(), SPH_O_NEW, 0644 );
		if ( m_iLockFD<0 )
		{
			m_sLastError.SetSprintf ( "failed to open %s: %s", sName.cstr(), strerror(errno) );
			sphLogDebug ( "failed to open %s: %s", sName.cstr(), strerror(errno) );
			return false;
		}
	}

	if ( !sphLockEx ( m_iLockFD, false ) )
	{
		m_sLastError.SetSprintf ( "failed to lock %s: %s", sName.cstr(), strerror(errno) );
		::close ( m_iLockFD );
		m_iLockFD = -1;
		return false;
	}
	sphLogDebug ( "lock %s success", sName.cstr() );
	return true;
}


void CSphIndex_VLN::Unlock()
{
	CSphString sName = GetIndexFileName("spl");
	sphLogDebug ( "Unlocking the index (lock %s)", sName.cstr() );
	if ( m_iLockFD>=0 )
	{
		sphLogDebug ( "File ID ok, closing lock FD %d, unlinking %s", m_iLockFD, sName.cstr() );
		sphLockUn ( m_iLockFD );
		::close ( m_iLockFD );
		::unlink ( sName.cstr() );
		m_iLockFD = -1;
	}
}


bool CSphIndex_VLN::Mlock ()
{
	bool bRes = true;
	if ( m_bPreloadWordlist )
		bRes &= m_tWordlist.m_pBuf.Mlock ( "wordlist", m_sLastError );

	if ( m_bOndiskAllAttr )
		return bRes;

	bRes &= m_dAttrShared.Mlock ( "docinfo", m_sLastError );

	if ( !m_bOndiskPoolAttr )
	{
		bRes &= m_dMvaShared.Mlock ( "mva", m_sLastError );
		bRes &= m_dStringShared.Mlock ( "strings", m_sLastError );
	}
	return bRes;
}


void CSphIndex_VLN::Dealloc ()
{
	if ( !m_bPreallocated )
		return;

	m_tDoclistFile.Close ();
	m_tHitlistFile.Close ();
	m_pDocinfoHash.Reset ();
	m_dAttrShared.Reset ();
	m_dMvaShared.Reset ();
	m_dStringShared.Reset ();
	m_pKillList.Reset ();
	m_tWordlist.Reset ();
	m_pSkiplists.Reset ();
	m_dAttrMapped.Close();
	m_dMvaMapped.Close();
	m_dStringMapped.Close();

	m_iDocinfo = 0;
	m_uMinMaxIndex = 0;
	m_tSettings.m_eDocinfo = SPH_DOCINFO_NONE;

	m_bPreallocated = false;
	SafeDelete ( m_pTokenizer );
	SafeDelete ( m_pDict );

	if ( m_iIndexTag>=0 && g_pMvaArena )
		g_MvaArena.TaggedFreeTag ( m_iIndexTag );
	m_iIndexTag = -1;

	m_pPreread = NULL;
	m_pAttrsStatus = NULL;

#ifndef NDEBUG
	m_dShared.Reset ();
#endif
}


void CSphIndex_VLN::SetEnableOndiskAttributes ( bool bPool )
{
	if ( m_bPreallocated )
		return;

	m_bOndiskAllAttr = !bPool;
	m_bOndiskPoolAttr = bPool;
}


void LoadIndexSettings ( CSphIndexSettings & tSettings, CSphReader & tReader, DWORD uVersion )
{
	if ( uVersion>=8 )
	{
		tSettings.m_iMinPrefixLen = tReader.GetDword ();
		tSettings.m_iMinInfixLen = tReader.GetDword ();

	} else if ( uVersion>=6 )
	{
		bool bPrefixesOnly = ( tReader.GetByte ()!=0 );
		tSettings.m_iMinPrefixLen = tReader.GetDword ();
		tSettings.m_iMinInfixLen = 0;
		if ( !bPrefixesOnly )
			Swap ( tSettings.m_iMinPrefixLen, tSettings.m_iMinInfixLen );
	}

	if ( uVersion>=38 )
		tSettings.m_iMaxSubstringLen = tReader.GetDword();

	if ( uVersion>=9 )
	{
		tSettings.m_bHtmlStrip = !!tReader.GetByte ();
		tSettings.m_sHtmlIndexAttrs = tReader.GetString ();
		tSettings.m_sHtmlRemoveElements = tReader.GetString ();
	}

	if ( uVersion>=12 )
		tSettings.m_bIndexExactWords = !!tReader.GetByte ();

	if ( uVersion>=18 )
		tSettings.m_eHitless = (ESphHitless)tReader.GetDword();

	if ( uVersion>=19 )
		tSettings.m_eHitFormat = (ESphHitFormat)tReader.GetDword();
	else // force plain format for old indices
		tSettings.m_eHitFormat = SPH_HIT_FORMAT_PLAIN;

	if ( uVersion>=21 )
		tSettings.m_bIndexSP = !!tReader.GetByte();

	if ( uVersion>=22 )
	{
		tSettings.m_sZones = tReader.GetString();
		if ( uVersion<25 && !tSettings.m_sZones.IsEmpty() )
			tSettings.m_sZones.SetSprintf ( "%s*", tSettings.m_sZones.cstr() );
	}

	if ( uVersion>=23 )
	{
		tSettings.m_iBoundaryStep = (int)tReader.GetDword();
		tSettings.m_iStopwordStep = (int)tReader.GetDword();
	}

	if ( uVersion>=28 )
		tSettings.m_iOvershortStep = (int)tReader.GetDword();

	if ( uVersion>=30 )
		tSettings.m_iEmbeddedLimit = (int)tReader.GetDword();

	if ( uVersion>=32 )
	{
		tSettings.m_eBigramIndex = (ESphBigram)tReader.GetByte();
		tSettings.m_sBigramWords = tReader.GetString();
	}

	if ( uVersion>=35 )
		tSettings.m_bIndexFieldLens = ( tReader.GetByte()!=0 );

	if ( uVersion>=39 )
	{
		tSettings.m_eChineseRLP = (ESphRLPFilter)tReader.GetByte();
		tSettings.m_sRLPContext = tReader.GetString();
	}
}


bool CSphIndex_VLN::LoadHeader ( const char * sHeaderName, bool bStripPath, CSphEmbeddedFiles & tEmbeddedFiles, CSphString & sWarning )
{
	const int MAX_HEADER_SIZE = 32768;
	CSphFixedVector<BYTE> dCacheInfo ( MAX_HEADER_SIZE );

	CSphAutoreader rdInfo ( dCacheInfo.Begin(), MAX_HEADER_SIZE ); // to avoid mallocs
	if ( !rdInfo.Open ( sHeaderName, m_sLastError ) )
		return false;

	// version
	DWORD uHeader = rdInfo.GetDword ();
	if ( uHeader!=INDEX_MAGIC_HEADER )
	{
		m_sLastError.SetSprintf ( "%s is invalid header file (too old index version?)", sHeaderName );
		return false;
	}

	m_uVersion = rdInfo.GetDword();
	if ( m_uVersion==0 || m_uVersion>INDEX_FORMAT_VERSION )
	{
		m_sLastError.SetSprintf ( "%s is v.%d, binary is v.%d", sHeaderName, m_uVersion, INDEX_FORMAT_VERSION );
		return false;
	}

	// bits
	m_bUse64 = false;
	if ( m_uVersion>=2 )
		m_bUse64 = ( rdInfo.GetDword ()!=0 );

	if ( m_bUse64!=USE_64BIT )
	{
#if USE_64BIT
		// TODO: may be do this param conditional and push it into the config?
		m_bId32to64 = true;

		if ( m_bOndiskAllAttr || m_bOndiskPoolAttr )
		{
			m_bOndiskAllAttr = false;
			m_bOndiskPoolAttr = false;
			sWarning.SetSprintf ( "can not convert attributes, ondisk_attrs disabled" );
		}
#else
		m_sLastError.SetSprintf ( "'%s' is id%d, and this binary is id%d",
			GetIndexFileName("sph").cstr(),
			m_bUse64 ? 64 : 32, USE_64BIT ? 64 : 32 );
		return false;
#endif
	}

	// FIXME!!! old index min-max precompute
	if ( m_uVersion<20 && ( m_bOndiskAllAttr || m_bOndiskPoolAttr ) )
	{
		m_bOndiskPoolAttr = false;
		m_bOndiskAllAttr = false;
	}

	// skiplists
	m_bHaveSkips = ( m_uVersion>=31 );

	// docinfo
	m_tSettings.m_eDocinfo = (ESphDocinfo) rdInfo.GetDword();

	// schema
	// 4th arg means that inline attributes need be dynamic in searching time too
	ReadSchema ( rdInfo, m_tSchema, m_uVersion, m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE );

	// check schema for dupes
	for ( int iAttr=1; iAttr<m_tSchema.GetAttrsCount(); iAttr++ )
	{
		const CSphColumnInfo & tCol = m_tSchema.GetAttr(iAttr);
		for ( int i=0; i<iAttr; i++ )
			if ( m_tSchema.GetAttr(i).m_sName==tCol.m_sName )
				sWarning.SetSprintf ( "duplicate attribute name: %s", tCol.m_sName.cstr() );
	}

	// in case of *fork rotation we reuse min match from 1st rotated index ( it could be less than my size and inline ( m_pDynamic ) )
	// min doc
	m_dMinRow.Reset ( m_tSchema.GetRowSize() );
	if ( m_uVersion>=2 )
		m_iMinDocid = (SphDocID_t) rdInfo.GetOffset (); // v2+; losing high bits when !USE_64 is intentional, check is performed on bUse64 above
	else
		m_iMinDocid = rdInfo.GetDword(); // v1
	if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE )
		rdInfo.GetBytes ( m_dMinRow.Begin(), sizeof(CSphRowitem)*m_tSchema.GetRowSize() );

	// dictionary header (wordlist checkpoints, infix blocks, etc)
	m_tWordlist.m_iDictCheckpointsOffset = rdInfo.GetOffset();
	m_tWordlist.m_iDictCheckpoints = rdInfo.GetDword();
	if ( m_uVersion>=27 )
	{
		m_tWordlist.m_iInfixCodepointBytes = rdInfo.GetByte();
		m_tWordlist.m_iInfixBlocksOffset = rdInfo.GetDword();
	}
	if ( m_uVersion>=34 )
		m_tWordlist.m_iInfixBlocksWordsSize = rdInfo.GetDword();

	m_tWordlist.m_dCheckpoints.Reset ( m_tWordlist.m_iDictCheckpoints );

	// index stats
	m_tStats.m_iTotalDocuments = rdInfo.GetDword ();
	m_tStats.m_iTotalBytes = rdInfo.GetOffset ();
	if ( m_uVersion>=40 )
		m_iTotalDups = rdInfo.GetDword();

	LoadIndexSettings ( m_tSettings, rdInfo, m_uVersion );
	if ( m_uVersion<9 )
		m_bStripperInited = false;

	if ( m_uVersion>=9 )
	{
		// tokenizer stuff
		CSphTokenizerSettings tSettings;
		LoadTokenizerSettings ( rdInfo, tSettings, tEmbeddedFiles, m_uVersion, sWarning );

		if ( bStripPath )
			StripPath ( tSettings.m_sSynonymsFile );

		ISphTokenizer * pTokenizer = ISphTokenizer::Create ( tSettings, &tEmbeddedFiles, m_sLastError );
		if ( !pTokenizer )
			return false;

		// dictionary stuff
		CSphDictSettings tDictSettings;
		LoadDictionarySettings ( rdInfo, tDictSettings, tEmbeddedFiles, m_uVersion, sWarning );
		if ( m_bId32to64 )
			tDictSettings.m_bCrc32 = true;

		if ( bStripPath )
		{
			StripPath ( tDictSettings.m_sStopwords );
			ARRAY_FOREACH ( i, tDictSettings.m_dWordforms )
				StripPath ( tDictSettings.m_dWordforms[i] );
		}

		CSphDict * pDict = tDictSettings.m_bWordDict
			? sphCreateDictionaryKeywords ( tDictSettings, &tEmbeddedFiles, pTokenizer, m_sIndexName.cstr(), m_sLastError )
			: sphCreateDictionaryCRC ( tDictSettings, &tEmbeddedFiles, pTokenizer, m_sIndexName.cstr(), m_sLastError );

		if ( !pDict )
			return false;

		if ( tDictSettings.m_sMorphFingerprint!=pDict->GetMorphDataFingerprint() )
			sWarning.SetSprintf ( "different lemmatizer dictionaries (index='%s', current='%s')",
				tDictSettings.m_sMorphFingerprint.cstr(),
				pDict->GetMorphDataFingerprint().cstr() );

		SetDictionary ( pDict );

		pTokenizer = ISphTokenizer::CreateMultiformFilter ( pTokenizer, pDict->GetMultiWordforms () );

#if USE_RLP
		pTokenizer = ISphTokenizer::CreateRLPFilter ( pTokenizer, m_tSettings.m_eChineseRLP!=SPH_RLP_NONE, g_sRLPRoot.cstr(),
			g_sRLPEnv.cstr(), m_tSettings.m_sRLPContext.cstr(), true, m_sLastError );

		if ( !pTokenizer )
			return false;
#endif

		SetTokenizer ( pTokenizer );
		SetupQueryTokenizer();

		// initialize AOT if needed
		CSphVector<CSphString> dMorphs;
		sphSplit ( dMorphs, tDictSettings.m_sMorphology.cstr() );
		m_tSettings.m_uAotFilterMask = 0;
		for ( int j=0; j<AOT_LENGTH; ++j )
		{
			char buf_all[20];
			sprintf ( buf_all, "lemmatize_%s_all", AOT_LANGUAGES[j] ); // NOLINT
			ARRAY_FOREACH ( i, dMorphs )
				if ( dMorphs[i]==buf_all )
				{
					m_tSettings.m_uAotFilterMask |= (1UL) << j;
					break;
				}
		}

	} else
	{
		if ( m_bId32to64 )
		{
			m_sLastError.SetSprintf ( "too old id32 index; can not be loaded by this id64 binary" );
			return false;
		}
	}

	if ( m_uVersion>=10 )
		m_iKillListSize = rdInfo.GetDword ();

	if ( m_uVersion>=33 )
		m_uMinMaxIndex = rdInfo.GetOffset ();
	else if ( m_uVersion>=20 )
		m_uMinMaxIndex = rdInfo.GetDword ();

	if ( m_uVersion>=28 )
	{
		CSphFieldFilterSettings tFieldFilterSettings;
		LoadFieldFilterSettings ( rdInfo, tFieldFilterSettings );
		if ( tFieldFilterSettings.m_dRegexps.GetLength() )
			SetFieldFilter ( sphCreateFieldFilter ( tFieldFilterSettings, sWarning ) );
	}

	if ( m_uVersion>=35 && m_tSettings.m_bIndexFieldLens )
		ARRAY_FOREACH ( i, m_tSchema.m_dFields )
			m_dFieldLens[i] = rdInfo.GetOffset(); // FIXME? ideally 64bit even when off is 32bit..

	// post-load stuff.. for now, bigrams
	CSphIndexSettings & s = m_tSettings;
	if ( s.m_eBigramIndex!=SPH_BIGRAM_NONE && s.m_eBigramIndex!=SPH_BIGRAM_ALL )
	{
		BYTE * pTok;
		m_pTokenizer->SetBuffer ( (BYTE*)s.m_sBigramWords.cstr(), s.m_sBigramWords.Length() );
		while ( ( pTok = m_pTokenizer->GetToken() )!=NULL )
			s.m_dBigramWords.Add() = (const char*)pTok;
		s.m_dBigramWords.Sort();
	}


	if ( rdInfo.GetErrorFlag() )
		m_sLastError.SetSprintf ( "%s: failed to parse header (unexpected eof)", sHeaderName );
	return !rdInfo.GetErrorFlag();
}


void CSphIndex_VLN::DebugDumpHeader ( FILE * fp, const char * sHeaderName, bool bConfig )
{
	CSphEmbeddedFiles tEmbeddedFiles;
	CSphString sWarning;
	if ( !LoadHeader ( sHeaderName, false, tEmbeddedFiles, sWarning ) )
	{
		fprintf ( fp, "FATAL: failed to load header: %s.\n", m_sLastError.cstr() );
		return;
	}

	if ( !sWarning.IsEmpty () )
		fprintf ( fp, "WARNING: %s\n", sWarning.cstr () );

	///////////////////////////////////////////////
	// print header in index config section format
	///////////////////////////////////////////////

	if ( bConfig )
	{
		fprintf ( fp, "\nsource $dump\n{\n" );

		fprintf ( fp, "\tsql_query = SELECT id \\\n" );
		ARRAY_FOREACH ( i, m_tSchema.m_dFields )
			fprintf ( fp, "\t, %s \\\n", m_tSchema.m_dFields[i].m_sName.cstr() );
		for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
		{
			const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
			fprintf ( fp, "\t, %s \\\n", tAttr.m_sName.cstr() );
		}
		fprintf ( fp, "\tFROM documents\n" );

		if ( m_tSchema.GetAttrsCount() )
			fprintf ( fp, "\n" );

		for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
		{
			const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
			if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET )
				fprintf ( fp, "\tsql_attr_multi = uint %s from field\n", tAttr.m_sName.cstr() );
			else if ( tAttr.m_eAttrType==SPH_ATTR_INT64SET )
				fprintf ( fp, "\tsql_attr_multi = bigint %s from field\n", tAttr.m_sName.cstr() );
			else if ( tAttr.m_eAttrType==SPH_ATTR_INTEGER && tAttr.m_tLocator.IsBitfield() )
				fprintf ( fp, "\tsql_attr_uint = %s:%d\n", tAttr.m_sName.cstr(), tAttr.m_tLocator.m_iBitCount );
			else if ( tAttr.m_eAttrType==SPH_ATTR_TOKENCOUNT )
			{; // intendedly skip, as these are autogenerated by index_field_lengths=1
			} else
				fprintf ( fp, "\t%s = %s\n", sphTypeDirective ( tAttr.m_eAttrType ), tAttr.m_sName.cstr() );
		}

		fprintf ( fp, "}\n\nindex $dump\n{\n\tsource = $dump\n\tpath = $dump\n" );

		if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE )
			fprintf ( fp, "\tdocinfo = inline\n" );
		if ( m_tSettings.m_iMinPrefixLen )
			fprintf ( fp, "\tmin_prefix_len = %d\n", m_tSettings.m_iMinPrefixLen );
		if ( m_tSettings.m_iMinInfixLen )
			fprintf ( fp, "\tmin_prefix_len = %d\n", m_tSettings.m_iMinInfixLen );
		if ( m_tSettings.m_iMaxSubstringLen )
			fprintf ( fp, "\tmax_substring_len = %d\n", m_tSettings.m_iMaxSubstringLen );
		if ( m_tSettings.m_bIndexExactWords )
			fprintf ( fp, "\tindex_exact_words = %d\n", m_tSettings.m_bIndexExactWords ? 1 : 0 );
		if ( m_tSettings.m_bHtmlStrip )
			fprintf ( fp, "\thtml_strip = 1\n" );
		if ( !m_tSettings.m_sHtmlIndexAttrs.IsEmpty() )
			fprintf ( fp, "\thtml_index_attrs = %s\n", m_tSettings.m_sHtmlIndexAttrs.cstr () );
		if ( !m_tSettings.m_sHtmlRemoveElements.IsEmpty() )
			fprintf ( fp, "\thtml_remove_elements = %s\n", m_tSettings.m_sHtmlRemoveElements.cstr () );
		if ( m_tSettings.m_sZones.cstr() )
			fprintf ( fp, "\tindex_zones = %s\n", m_tSettings.m_sZones.cstr() );
		if ( m_tSettings.m_bIndexFieldLens )
			fprintf ( fp, "\tindex_field_lengths = 1\n" );

		if ( m_pTokenizer )
		{
			const CSphTokenizerSettings & tSettings = m_pTokenizer->GetSettings ();
			fprintf ( fp, "\tcharset_type = %s\n", tSettings.m_iType==TOKENIZER_SBCS ? "sbcs" : "utf-8" );
			fprintf ( fp, "\tcharset_table = %s\n", tSettings.m_sCaseFolding.cstr () );
			if ( tSettings.m_iMinWordLen>1 )
				fprintf ( fp, "\tmin_word_len = %d\n", tSettings.m_iMinWordLen );
			if ( tSettings.m_iNgramLen && !tSettings.m_sNgramChars.IsEmpty() )
				fprintf ( fp, "\tngram_len = %d\nngram_chars = %s\n",
					tSettings.m_iNgramLen, tSettings.m_sNgramChars.cstr () );
			if ( !tSettings.m_sSynonymsFile.IsEmpty() )
				fprintf ( fp, "\texceptions = %s\n", tSettings.m_sSynonymsFile.cstr () );
			if ( !tSettings.m_sBoundary.IsEmpty() )
				fprintf ( fp, "\tphrase_boundary = %s\n", tSettings.m_sBoundary.cstr () );
			if ( !tSettings.m_sIgnoreChars.IsEmpty() )
				fprintf ( fp, "\tignore_chars = %s\n", tSettings.m_sIgnoreChars.cstr () );
			if ( !tSettings.m_sBlendChars.IsEmpty() )
				fprintf ( fp, "\tblend_chars = %s\n", tSettings.m_sBlendChars.cstr () );
			if ( !tSettings.m_sBlendMode.IsEmpty() )
				fprintf ( fp, "\tblend_mode = %s\n", tSettings.m_sBlendMode.cstr () );
		}

		if ( m_pDict )
		{
			const CSphDictSettings & tSettings = m_pDict->GetSettings ();
			if ( tSettings.m_bWordDict )
				fprintf ( fp, "\tdict = keywords\n" );
			if ( !tSettings.m_sMorphology.IsEmpty() )
				fprintf ( fp, "\tmorphology = %s\n", tSettings.m_sMorphology.cstr () );
			if ( !tSettings.m_sStopwords.IsEmpty() )
				fprintf ( fp, "\tstopwords = %s\n", tSettings.m_sStopwords.cstr () );
			if ( tSettings.m_dWordforms.GetLength() )
			{
				fprintf ( fp, "\twordforms =" );
				ARRAY_FOREACH ( i, tSettings.m_dWordforms )
					fprintf ( fp, " %s", tSettings.m_dWordforms[i].cstr () );
				fprintf ( fp, "\n" );
			}
			if ( tSettings.m_iMinStemmingLen>1 )
				fprintf ( fp, "\tmin_stemming_len = %d\n", tSettings.m_iMinStemmingLen );
		}

		fprintf ( fp, "}\n" );
		return;
	}

	///////////////////////////////////////////////
	// print header and stats in "readable" format
	///////////////////////////////////////////////

	fprintf ( fp, "version: %d\n",			m_uVersion );
	fprintf ( fp, "idbits: %d\n",			m_bUse64 ? 64 : 32 );
	fprintf ( fp, "docinfo: " );
	switch ( m_tSettings.m_eDocinfo )
	{
		case SPH_DOCINFO_NONE:		fprintf ( fp, "none\n" ); break;
		case SPH_DOCINFO_INLINE:	fprintf ( fp, "inline\n" ); break;
		case SPH_DOCINFO_EXTERN:	fprintf ( fp, "extern\n" ); break;
		default:					fprintf ( fp, "unknown (value=%d)\n", m_tSettings.m_eDocinfo ); break;
	}

	fprintf ( fp, "fields: %d\n", m_tSchema.m_dFields.GetLength() );
	ARRAY_FOREACH ( i, m_tSchema.m_dFields )
		fprintf ( fp, "  field %d: %s\n", i, m_tSchema.m_dFields[i].m_sName.cstr() );

	fprintf ( fp, "attrs: %d\n", m_tSchema.GetAttrsCount() );
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
		fprintf ( fp, "  attr %d: %s, %s", i, tAttr.m_sName.cstr(), sphTypeName ( tAttr.m_eAttrType ) );
		if ( tAttr.m_eAttrType==SPH_ATTR_INTEGER && tAttr.m_tLocator.m_iBitCount!=32 )
			fprintf ( fp, ", bits %d", tAttr.m_tLocator.m_iBitCount );
		fprintf ( fp, ", bitoff %d\n", tAttr.m_tLocator.m_iBitOffset );
	}

	// skipped min doc, wordlist checkpoints
	fprintf ( fp, "total-documents: "INT64_FMT"\n", m_tStats.m_iTotalDocuments );
	fprintf ( fp, "total-bytes: "INT64_FMT"\n", int64_t(m_tStats.m_iTotalBytes) );
	fprintf ( fp, "total-duplicates: %d\n", m_iTotalDups );

	fprintf ( fp, "min-prefix-len: %d\n", m_tSettings.m_iMinPrefixLen );
	fprintf ( fp, "min-infix-len: %d\n", m_tSettings.m_iMinInfixLen );
	fprintf ( fp, "max-substring-len: %d\n", m_tSettings.m_iMaxSubstringLen );
	fprintf ( fp, "exact-words: %d\n", m_tSettings.m_bIndexExactWords ? 1 : 0 );
	fprintf ( fp, "html-strip: %d\n", m_tSettings.m_bHtmlStrip ? 1 : 0 );
	fprintf ( fp, "html-index-attrs: %s\n", m_tSettings.m_sHtmlIndexAttrs.cstr () );
	fprintf ( fp, "html-remove-elements: %s\n", m_tSettings.m_sHtmlRemoveElements.cstr () );
	fprintf ( fp, "index-zones: %s\n", m_tSettings.m_sZones.cstr() );

	if ( m_pTokenizer )
	{
		const CSphTokenizerSettings & tSettings = m_pTokenizer->GetSettings ();
		fprintf ( fp, "tokenizer-type: %d\n", tSettings.m_iType );
		fprintf ( fp, "tokenizer-case-folding: %s\n", tSettings.m_sCaseFolding.cstr () );
		fprintf ( fp, "tokenizer-min-word-len: %d\n", tSettings.m_iMinWordLen );
		fprintf ( fp, "tokenizer-ngram-chars: %s\n", tSettings.m_sNgramChars.cstr () );
		fprintf ( fp, "tokenizer-ngram-len: %d\n", tSettings.m_iNgramLen );
		fprintf ( fp, "tokenizer-exceptions: %s\n", tSettings.m_sSynonymsFile.cstr () );
		fprintf ( fp, "tokenizer-phrase-boundary: %s\n", tSettings.m_sBoundary.cstr () );
		fprintf ( fp, "tokenizer-ignore-chars: %s\n", tSettings.m_sIgnoreChars.cstr () );
		fprintf ( fp, "tokenizer-blend-chars: %s\n", tSettings.m_sBlendChars.cstr () );
		fprintf ( fp, "tokenizer-blend-mode: %s\n", tSettings.m_sBlendMode.cstr () );

		fprintf ( fp, "tokenizer-exceptions: %s\n", tSettings.m_sSynonymsFile.cstr () );
		fprintf ( fp, "dictionary-embedded-exceptions: %d\n", tEmbeddedFiles.m_bEmbeddedSynonyms ? 1 : 0 );
		if ( tEmbeddedFiles.m_bEmbeddedSynonyms )
		{
			ARRAY_FOREACH ( i, tEmbeddedFiles.m_dSynonyms )
				fprintf ( fp, "\tdictionary-embedded-exception [%d]: %s\n", i, tEmbeddedFiles.m_dSynonyms[i].cstr () );
		}
	}

	if ( m_pDict )
	{
		const CSphDictSettings & tSettings = m_pDict->GetSettings ();
		fprintf ( fp, "dict: %s\n", tSettings.m_bWordDict ? "keywords" : "crc" );
		fprintf ( fp, "dictionary-morphology: %s\n", tSettings.m_sMorphology.cstr () );

		fprintf ( fp, "dictionary-stopwords-file: %s\n", tSettings.m_sStopwords.cstr () );
		fprintf ( fp, "dictionary-embedded-stopwords: %d\n", tEmbeddedFiles.m_bEmbeddedStopwords ? 1 : 0 );
		if ( tEmbeddedFiles.m_bEmbeddedStopwords )
		{
			ARRAY_FOREACH ( i, tEmbeddedFiles.m_dStopwords )
				fprintf ( fp, "\tdictionary-embedded-stopword [%d]: "DOCID_FMT"\n", i, tEmbeddedFiles.m_dStopwords[i] );
		}

		ARRAY_FOREACH ( i, tSettings.m_dWordforms )
			fprintf ( fp, "dictionary-wordform-file [%d]: %s\n", i, tSettings.m_dWordforms[i].cstr () );

		fprintf ( fp, "dictionary-embedded-wordforms: %d\n", tEmbeddedFiles.m_bEmbeddedWordforms ? 1 : 0 );
		if ( tEmbeddedFiles.m_bEmbeddedWordforms )
		{
			ARRAY_FOREACH ( i, tEmbeddedFiles.m_dWordforms )
				fprintf ( fp, "\tdictionary-embedded-wordform [%d]: %s\n", i, tEmbeddedFiles.m_dWordforms[i].cstr () );
		}

		fprintf ( fp, "min-stemming-len: %d\n", tSettings.m_iMinStemmingLen );
	}

	fprintf ( fp, "killlist-size: %d\n", m_iKillListSize );
	fprintf ( fp, "min-max-index: "UINT64_FMT"\n", m_uMinMaxIndex );

	if ( m_pFieldFilter )
	{
		CSphFieldFilterSettings tSettings;
		m_pFieldFilter->GetSettings ( tSettings );
		fprintf ( fp, "field-filter-utf8: %d\n", tSettings.m_bUTF8 ? 1 : 0 );
		ARRAY_FOREACH ( i, tSettings.m_dRegexps )
			fprintf ( fp, "field-filter-regexp [%d]: %s\n", i, tSettings.m_dRegexps[i].cstr() );
	}
}


void CSphIndex_VLN::DebugDumpDocids ( FILE * fp )
{
	if ( m_tSettings.m_eDocinfo!=SPH_DOCINFO_EXTERN )
	{
		fprintf ( fp, "FATAL: docids dump only supported for docinfo=extern\n" );
		return;
	}

	const int iRowStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();

	const int64_t iNumMinMaxRow = ( m_uVersion>=20 ) ? ( (m_iDocinfoIndex+1)*iRowStride*2 ) : 0;
	const int64_t iNumRows = (m_tAttr.GetNumEntries()-iNumMinMaxRow) / iRowStride;

	const int64_t iDocinfoSize = iRowStride*m_iDocinfo*sizeof(DWORD);
	const int64_t iMinmaxSize = iNumMinMaxRow*sizeof(CSphRowitem);

	fprintf ( fp, "docinfo-bytes: docinfo="INT64_FMT", min-max="INT64_FMT", total="UINT64_FMT"\n"
		, iDocinfoSize, iMinmaxSize, (uint64_t)m_tAttr.GetLengthBytes() );
	fprintf ( fp, "docinfo-stride: %d\n", (int)(iRowStride*sizeof(DWORD)) );
	fprintf ( fp, "docinfo-rows: "INT64_FMT"\n", iNumRows );

	if ( !m_tAttr.GetNumEntries() )
		return;

	DWORD * pDocinfo = m_tAttr.GetWritePtr();
	for ( int64_t iRow=0; iRow<iNumRows; iRow++, pDocinfo+=iRowStride )
		printf ( INT64_FMT". id=" DOCID_FMT "\n", iRow+1, DOCINFO2ID ( pDocinfo ) );
	printf ( "--- min-max="INT64_FMT" ---\n", iNumMinMaxRow );
	for ( int64_t iRow=0; iRow<(m_iDocinfoIndex+1)*2; iRow++, pDocinfo+=iRowStride )
		printf ( "id=" DOCID_FMT "\n", DOCINFO2ID ( pDocinfo ) );
}


void CSphIndex_VLN::DebugDumpHitlist ( FILE * fp, const char * sKeyword, bool bID )
{
	WITH_QWORD ( this, false, Qword, DumpHitlist<Qword> ( fp, sKeyword, bID ) );
}


template < class Qword >
void CSphIndex_VLN::DumpHitlist ( FILE * fp, const char * sKeyword, bool bID )
{
	// get keyword id
	SphWordID_t uWordID = 0;
	BYTE * sTok = NULL;
	if ( !bID )
	{
		CSphString sBuf ( sKeyword );

		m_pTokenizer->SetBuffer ( (BYTE*)sBuf.cstr(), strlen ( sBuf.cstr() ) );
		sTok = m_pTokenizer->GetToken();

		if ( !sTok )
			sphDie ( "keyword=%s, no token (too short?)", sKeyword );

		uWordID = m_pDict->GetWordID ( sTok );
		if ( !uWordID )
			sphDie ( "keyword=%s, tok=%s, no wordid (stopped?)", sKeyword, sTok );

		fprintf ( fp, "keyword=%s, tok=%s, wordid="UINT64_FMT"\n", sKeyword, sTok, uint64_t(uWordID) );

	} else
	{
		uWordID = (SphWordID_t) strtoull ( sKeyword, NULL, 10 );
		if ( !uWordID )
			sphDie ( "failed to convert keyword=%s to id (must be integer)", sKeyword );

		fprintf ( fp, "wordid="UINT64_FMT"\n", uint64_t(uWordID) );
	}

	// open files
	CSphAutofile tDoclist, tHitlist, tWordlist;
	if ( tDoclist.Open ( GetIndexFileName("spd"), SPH_O_READ, m_sLastError ) < 0 )
		sphDie ( "failed to open doclist: %s", m_sLastError.cstr() );

	if ( tHitlist.Open ( GetIndexFileName ( m_uVersion>=3 ? "spp" : "spd" ), SPH_O_READ, m_sLastError ) < 0 )
		sphDie ( "failed to open hitlist: %s", m_sLastError.cstr() );

	if ( tWordlist.Open ( GetIndexFileName ( "spi" ), SPH_O_READ, m_sLastError ) < 0 )
		sphDie ( "failed to open wordlist: %s", m_sLastError.cstr() );

	// aim
	DiskIndexQwordSetup_c tTermSetup ( tDoclist, tHitlist, tWordlist, m_bPreloadWordlist ? 0 : m_tWordlist.m_iMaxChunk, m_pSkiplists.GetWritePtr(), NULL );
	tTermSetup.m_pDict = m_pDict;
	tTermSetup.m_pIndex = this;
	tTermSetup.m_eDocinfo = m_tSettings.m_eDocinfo;
	tTermSetup.m_iMinDocid = m_iMinDocid;
	tTermSetup.m_pMinRow = m_dMinRow.Begin();
	tTermSetup.m_bSetupReaders = true;

	Qword tKeyword ( false, false );
	tKeyword.m_tDoc.m_iDocID = m_iMinDocid;
	tKeyword.m_iWordID = uWordID;
	tKeyword.m_sWord = sKeyword;
	tKeyword.m_sDictWord = (const char *)sTok;
	if ( !tTermSetup.QwordSetup ( &tKeyword ) )
		sphDie ( "failed to setup keyword" );

	int iSize = m_tSchema.GetRowSize();
	CSphVector<CSphRowitem> dAttrs ( iSize );

	// press play on tape
	for ( ;; )
	{
		tKeyword.GetNextDoc ( iSize ? &dAttrs[0] : NULL );
		if ( !tKeyword.m_tDoc.m_iDocID )
			break;
		tKeyword.SeekHitlist ( tKeyword.m_iHitlistPos );

		int iHits = 0;
		if ( tKeyword.m_bHasHitlist )
			for ( Hitpos_t uHit = tKeyword.GetNextHit(); uHit!=EMPTY_HIT; uHit = tKeyword.GetNextHit() )
			{
				fprintf ( fp, "doc="DOCID_FMT", hit=0x%08x\n", tKeyword.m_tDoc.m_iDocID, uHit ); // FIXME?
				iHits++;
			}

		if ( !iHits )
		{
			uint64_t uOff = tKeyword.m_iHitlistPos;
			fprintf ( fp, "doc="DOCID_FMT", NO HITS, inline=%d, off="UINT64_FMT"\n",
				tKeyword.m_tDoc.m_iDocID, (int)(uOff>>63), (uOff<<1)>>1 );
		}
	}
}


void CSphIndex_VLN::DebugDumpDict ( FILE * fp )
{
	if ( !m_pDict->GetSettings().m_bWordDict )
	{
		fprintf ( fp, "sorry, DebugDumpDict() only supports dict=keywords for now\n" );
		return;
	}

	// thread safe outer storage for dictionaries chunks and file
	// FIXME! cut-n-paste
	CSphString sError;
	BYTE * pBuf = NULL;
	int iFD = -1;
	CSphAutofile rdWordlist;
	if ( !m_bPreloadWordlist )
	{
		if ( m_bKeepFilesOpen )
			iFD = m_tWordlist.m_tFile.GetFD();
		else
		{
			iFD = rdWordlist.Open ( GetIndexFileName ( "spi" ), SPH_O_READ, sError );
			if ( iFD<0 )
			{
				fprintf ( fp, "ERROR: %s\n", sError.cstr() );
				return;
			}
		}
		if ( m_tWordlist.m_iMaxChunk>0 )
			pBuf = new BYTE [ m_tWordlist.m_iMaxChunk ];
	}

	fprintf ( fp, "keyword,docs,hits,offset\n" );
	ARRAY_FOREACH ( i, m_tWordlist.m_dCheckpoints )
	{
		KeywordsBlockReader_c tCtx ( m_tWordlist.AcquireDict ( &m_tWordlist.m_dCheckpoints[i], iFD, pBuf ), m_bHaveSkips );
		while ( tCtx.UnpackWord() )
			fprintf ( fp, "%s,%d,%d," INT64_FMT "\n", tCtx.GetWord(), tCtx.m_iDocs, tCtx.m_iHits, int64_t(tCtx.m_iDoclistOffset) );
	}
}

//////////////////////////////////////////////////////////////////////////

bool CSphIndex_VLN::Prealloc ( bool bMlock, bool bStripPath, CSphString & sWarning )
{
	MEMORY ( SPH_MEM_IDX_DISK );

	// reset
	Dealloc ();

	// always keep shared variables flag
	if ( m_dShared.IsEmpty() )
	{
		if ( !m_dShared.Alloc ( SPH_SHARED_VARS_COUNT, m_sLastError, sWarning ) )
			return false;
	}
	memset ( m_dShared.GetWritePtr(), 0, m_dShared.GetLengthBytes() );
	m_pPreread = m_dShared.GetWritePtr()+0;
	m_pAttrsStatus = m_dShared.GetWritePtr()+1;

	// set new locking flag
	m_tWordlist.m_pBuf.SetMlock ( bMlock );
	m_dAttrShared.SetMlock ( bMlock );
	m_dMvaShared.SetMlock ( bMlock );
	m_dStringShared.SetMlock ( bMlock );
	m_pKillList.SetMlock ( bMlock );
	m_pSkiplists.SetMlock ( bMlock );

	CSphEmbeddedFiles tEmbeddedFiles;

	// preload schema
	if ( !LoadHeader ( GetIndexFileName("sph").cstr(), bStripPath, tEmbeddedFiles, sWarning ) )
		return false;

	tEmbeddedFiles.Reset();

	// verify that data files are readable
	if ( !sphIsReadable ( GetIndexFileName("spd").cstr(), &m_sLastError ) )
		return false;

	if ( m_uVersion>=3 && !sphIsReadable ( GetIndexFileName("spp").cstr(), &m_sLastError ) )
		return false;

	if ( m_bHaveSkips && !sphIsReadable ( GetIndexFileName("spe").cstr(), &m_sLastError ) )
		return false;

	/////////////////////
	// prealloc wordlist
	/////////////////////

	// try to open wordlist file in all cases
	CSphAutofile tWordlist ( GetIndexFileName("spi"), SPH_O_READ, m_sLastError );
	if ( tWordlist.GetFD()<0 )
		return false;

	m_tWordlist.m_iSize = tWordlist.GetSize ( 1, true, m_sLastError );
	if ( m_tWordlist.m_iSize<0 )
		return false;

	if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN )
	{
		CSphAutofile tDocinfo ( GetIndexFileName("spa"), SPH_O_READ, m_sLastError );
		if ( tDocinfo.GetFD()<0 )
			return false;

		m_bIsEmpty = ( tDocinfo.GetSize ( 0, false, m_sLastError )==0 );
	} else
		m_bIsEmpty = ( m_tWordlist.m_iSize<=1 );

	if ( ( m_tWordlist.m_iSize<=1 )!=( m_tWordlist.m_dCheckpoints.GetLength()==0 ) )
		sphWarning ( "wordlist size mismatch (size="INT64_FMT", checkpoints=%d)", m_tWordlist.m_iSize, m_tWordlist.m_dCheckpoints.GetLength() );

	// make sure checkpoints are loadable
	// pre-11 indices use different offset type (this is fixed up later during the loading)
	assert ( m_tWordlist.m_iDictCheckpointsOffset>0 );

	// prealloc wordlist upto checkpoints
	// (keyword blocks aka checkpoints, infix blocks etc will be loaded separately)
	if ( m_bPreloadWordlist )
		if ( !m_tWordlist.m_pBuf.Alloc ( m_tWordlist.m_iDictCheckpointsOffset, m_sLastError, sWarning ) )
			return false;

	// preopen
	if ( m_bKeepFilesOpen )
	{
		if ( m_tDoclistFile.Open ( GetIndexFileName("spd"), SPH_O_READ, m_sLastError ) < 0 )
			return false;

		if ( m_tHitlistFile.Open ( GetIndexFileName ( m_uVersion>=3 ? "spp" : "spd" ), SPH_O_READ, m_sLastError ) < 0 )
			return false;

		if ( !m_bPreloadWordlist && m_tWordlist.m_tFile.Open ( GetIndexFileName("spi"), SPH_O_READ, m_sLastError ) < 0 )
			return false;
	}

	/////////////////////
	// prealloc docinfos
	/////////////////////

	if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN && !m_bIsEmpty )
	{
		/////////////
		// attr data
		/////////////

		assert ( !( m_bOndiskAllAttr || m_bOndiskPoolAttr ) || ( m_uVersion>=20 && !m_bId32to64 ) );
		int iStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();

		if ( m_bOndiskAllAttr )
		{
			if ( !m_dAttrMapped.Setup ( GetIndexFileName("spa").cstr(), m_sLastError ) )
				return false;

			int64_t iDocinfoSize = m_dAttrMapped.GetLengthBytes();
			if ( iDocinfoSize<0 )
				return false;
			iDocinfoSize = iDocinfoSize / sizeof(DWORD);
			int64_t iRealDocinfoSize = m_uMinMaxIndex ? m_uMinMaxIndex : iDocinfoSize;
			m_iDocinfo = iRealDocinfoSize / iStride;

			if ( !CheckDocsCount ( m_iDocinfo, m_sLastError ) )
				return false;

			if ( iDocinfoSize < iRealDocinfoSize )
			{
				m_sLastError.SetSprintf ( "precomputed chunk size check mismatch" );
				sphLogDebug ( "precomputed chunk size check mismatch (size="INT64_FMT", real="INT64_FMT", min-max="INT64_FMT", count="INT64_FMT")",
					iDocinfoSize, iRealDocinfoSize, m_uMinMaxIndex, m_iDocinfo );
				return false;
			}

			// FIXME!!! preload min-max index
			m_iDocinfoIndex = ( ( iDocinfoSize - iRealDocinfoSize ) / iStride / 2 ) - 1;
			m_pDocinfoIndex = m_dAttrMapped.GetWritePtr() + m_uMinMaxIndex;

			m_tAttr.Set ( m_dAttrMapped.GetWritePtr(), m_dAttrMapped.GetNumEntries() );
		} else
		{
			int iStride2 = iStride-1; // id64 - 1 DWORD = id32
			int iEntrySize = sizeof(DWORD)*iStride;

			CSphAutofile tDocinfo ( GetIndexFileName("spa"), SPH_O_READ, m_sLastError );
			if ( tDocinfo.GetFD()<0 )
				return false;

			int64_t iDocinfoSize = tDocinfo.GetSize ( iEntrySize, true, m_sLastError );
			if ( iDocinfoSize<0 )
				return false;
			iDocinfoSize = iDocinfoSize / sizeof(DWORD);
			int64_t iRealDocinfoSize = m_uMinMaxIndex ? m_uMinMaxIndex : iDocinfoSize;
			m_iDocinfo = iRealDocinfoSize / iStride;


			if ( m_bId32to64 )
			{
				// check also the case of id32 here, and correct m_iDocinfo for it
				m_iDocinfo = iRealDocinfoSize / iStride2;
				m_uMinMaxIndex = m_uMinMaxIndex / iStride2 * iStride;
			}

			if ( !CheckDocsCount ( m_iDocinfo, m_sLastError ) )
				return false;

			if ( m_uVersion < 20 )
			{
				if ( m_bId32to64 )
					iDocinfoSize = iDocinfoSize / iStride2 * iStride;
				m_iDocinfoIndex = ( m_iDocinfo+DOCINFO_INDEX_FREQ-1 ) / DOCINFO_INDEX_FREQ;

				// prealloc docinfo
				if ( !m_dAttrShared.Alloc ( iDocinfoSize + (m_iDocinfoIndex+1)*iStride*2 + ( m_bId32to64 ? m_iDocinfo : 0 ), m_sLastError, sWarning ) )
					return false;

				m_pDocinfoIndex = m_dAttrShared.GetWritePtr()+iDocinfoSize;
			} else
			{
				if ( iDocinfoSize < iRealDocinfoSize )
				{
					m_sLastError.SetSprintf ( "precomputed chunk size check mismatch" );
					sphLogDebug ( "precomputed chunk size check mismatch (size="INT64_FMT", real="INT64_FMT", min-max="INT64_FMT", count="INT64_FMT")",
						iDocinfoSize, iRealDocinfoSize, m_uMinMaxIndex, m_iDocinfo );
					return false;
				}

				m_iDocinfoIndex = ( ( iDocinfoSize - iRealDocinfoSize ) / (m_bId32to64?iStride2:iStride) / 2 ) - 1;

				// prealloc docinfo
				if ( !m_dAttrShared.Alloc ( iDocinfoSize + ( m_bId32to64 ? ( m_iDocinfo + m_iDocinfoIndex*2 + 2 ) : 0 ), m_sLastError, sWarning ) )
					return false;

				m_pDocinfoIndex = m_dAttrShared.GetWritePtr() + m_uMinMaxIndex;
			}

			m_tAttr.Set ( m_dAttrShared.GetWritePtr(), m_dAttrShared.GetNumEntries() );
		}

		// prealloc docinfo hash but only if docinfo is big enough (in other words if hash is 8x+ less in size)
		if ( m_pDocinfoHash.IsEmpty() && m_dAttrShared.GetLengthBytes() > ( 32 << DOCINFO_HASH_BITS ) && !g_bDebugCheck )
			if ( !m_pDocinfoHash.Alloc ( ( 1 << DOCINFO_HASH_BITS )+4, m_sLastError, sWarning ) )
				return false;


		////////////
		// MVA data
		////////////

		if ( m_uVersion>=4 )
		{
			if ( m_bOndiskAllAttr || m_bOndiskPoolAttr )
			{
				if ( !m_dMvaMapped.Setup ( GetIndexFileName("spm").cstr(), m_sLastError ) )
					return false;

				m_tMva.Set ( m_dMvaMapped.GetWritePtr(), m_dMvaMapped.GetNumEntries() );
			} else
			{
				// if index is v4, .spm must always exist, even though length could be 0
				CSphAutofile fdMva ( GetIndexFileName("spm"), SPH_O_READ, m_sLastError );
				if ( fdMva.GetFD()<0 )
					return false;

				SphOffset_t iMvaSize = fdMva.GetSize ( 0, true, m_sLastError );
				if ( iMvaSize<0 )
					return false;

				// prealloc
				if ( iMvaSize>0 )
					if ( !m_dMvaShared.Alloc ( DWORD(iMvaSize/sizeof(DWORD)), m_sLastError, sWarning ) )
						return false;

				m_tMva.Set ( m_dMvaShared.GetWritePtr(), m_dMvaShared.GetNumEntries() );
			}
		}

		///////////////
		// string data
		///////////////

		if ( m_uVersion>=17 )
		{
			if ( m_bOndiskAllAttr || m_bOndiskPoolAttr )
			{
				if ( !m_dStringMapped.Setup ( GetIndexFileName("sps").cstr(), m_sLastError ) )
					return false;

				m_tString.Set ( m_dStringMapped.GetWritePtr(), m_dStringMapped.GetNumEntries() );
			} else
			{
				CSphAutofile fdStrings ( GetIndexFileName("sps"), SPH_O_READ, m_sLastError );
				if ( fdStrings.GetFD()<0 )
					return false;

				SphOffset_t iStringsSize = fdStrings.GetSize ( 0, true, m_sLastError );
				if ( iStringsSize<0 )
					return false;

				// prealloc
				if ( iStringsSize>0 )
					if ( !m_dStringShared.Alloc ( DWORD(iStringsSize), m_sLastError, sWarning ) )
						return false;

				m_tString.Set ( m_dStringShared.GetWritePtr(), m_dStringShared.GetNumEntries() );
			}
		}
	}


	// prealloc killlist
	if ( m_uVersion>=10 )
	{
		CSphAutofile fdKillList ( GetIndexFileName("spk"), SPH_O_READ, m_sLastError );
		if ( fdKillList.GetFD()<0 )
			return false;

		SphOffset_t iSize = fdKillList.GetSize ( 0, true, m_sLastError );
		if ( iSize<0 )
			return false;

		if ( iSize!=(SphOffset_t)( m_iKillListSize*sizeof(SphAttr_t) ) )
		{
			m_sLastError.SetSprintf ( "header k-list size does not match .spk size (klist=" INT64_FMT ", spk=" INT64_FMT ")",
				(int64_t)( m_iKillListSize*sizeof(SphAttr_t) ),
				(int64_t) iSize );
			return false;
		}

		// prealloc
		if ( iSize>0 && !m_pKillList.Alloc ( m_iKillListSize, m_sLastError, sWarning ) )
			return false;
	}

	// prealloc skiplist
	if ( m_bHaveSkips )
	{
		CSphAutofile fdSkips ( GetIndexFileName("spe"), SPH_O_READ, m_sLastError );
		if ( fdSkips.GetFD()<0 )
			return false;

		SphOffset_t iSize = fdSkips.GetSize ( 0, true, m_sLastError );
		if ( iSize<0 )
			return false;

		if ( iSize>0 && !m_pSkiplists.Alloc ( iSize, m_sLastError, sWarning ) )
			return false;
	}

	bool bWordDict = false;
	if ( m_pDict )
		bWordDict = m_pDict->GetSettings().m_bWordDict;

	// preload checkpoints (must be done here as they are not shared)
	if 	( !m_tWordlist.ReadCP ( tWordlist, m_uVersion, bWordDict, m_sLastError ) )
	{
		m_sLastError.SetSprintf ( "failed to read %s: %s", GetIndexFileName("spi").cstr(), m_sLastError.cstr () );
		return false;
	}

	// all done
	m_bPreallocated = true;
	m_iIndexTag = ++m_iIndexTagSeq;
	return true;
}


template < typename T > bool CSphIndex_VLN::PrereadSharedBuffer ( CSphSharedBuffer<T> & pBuffer,
	const char * sExt, int64_t iExpected, int64_t iOffset )
{
	sphLogDebug ( "prereading .%s", sExt );

	if ( !pBuffer.GetLengthBytes() )
		return true;

	CSphAutofile fdBuf ( GetIndexFileName(sExt), SPH_O_READ, m_sLastError );
	if ( fdBuf.GetFD()<0 )
		return false;

	fdBuf.SetProgressCallback ( &m_tProgress );
	if ( iExpected==0 )
		iExpected = int64_t ( pBuffer.GetLengthBytes() ) - iOffset*sizeof(T);
	return fdBuf.Read ( pBuffer.GetWritePtr() + iOffset, iExpected, m_sLastError );
}


bool CSphIndex_VLN::Preread ()
{
	MEMORY ( SPH_MEM_IDX_DISK );

	sphLogDebug ( "CSphIndex_VLN::Preread invoked" );
	if ( !m_bPreallocated )
	{
		m_sLastError = "INTERNAL ERROR: not preallocated";
		return false;
	}
	if ( !m_pPreread || *m_pPreread )
	{
		m_sLastError = "INTERNAL ERROR: already preread";
		return false;
	}

	///////////////////
	// read everything
	///////////////////

	m_tProgress.m_ePhase = CSphIndexProgress::PHASE_PREREAD;
	m_tProgress.m_iBytes = 0;
	m_tProgress.m_iBytesTotal = m_pKillList.GetLengthBytes() + m_pSkiplists.GetLengthBytes();
	if ( !m_bOndiskAllAttr )
		m_tProgress.m_iBytesTotal += m_tAttr.GetLengthBytes();
	if ( !m_bOndiskAllAttr && !m_bOndiskPoolAttr )
		m_tProgress.m_iBytesTotal += m_tMva.GetLengthBytes() + m_tString.GetLengthBytes();

	if ( m_bPreloadWordlist )
		m_tProgress.m_iBytesTotal += m_tWordlist.m_pBuf.GetLengthBytes();

	int64_t iExpected = ( m_uVersion<20 ? m_iDocinfo * ( ( m_bId32to64 ? 1 : DOCINFO_IDSIZE ) + m_tSchema.GetRowSize() ) * sizeof(DWORD) : 0 );
	int64_t iOffset = ( m_bId32to64 ? ( m_iDocinfo + 2 + m_iDocinfoIndex * 2 ) : 0 );

	if ( !m_bOndiskAllAttr )
	{
		if ( !PrereadSharedBuffer ( m_dAttrShared, "spa", iExpected, iOffset ) )
				return false;
	}
	if ( !( m_bOndiskAllAttr || m_bOndiskPoolAttr ) )
	{
		if ( !PrereadSharedBuffer ( m_dMvaShared, "spm" ) )
			return false;
		if ( !PrereadSharedBuffer ( m_dStringShared, "sps" ) )
			return false;
	}

	if ( !PrereadSharedBuffer ( m_pKillList, "spk" ) )
		return false;
	if ( !PrereadSharedBuffer ( m_pSkiplists, "spe" ) )
		return false;

#if PARANOID
	for ( int i = 1; i < (int)m_iKillListSize; i++ )
		assert ( m_pKillList[i-1] < m_pKillList[i] );
#endif

	// preload wordlist
	// FIXME! OPTIMIZE! can skip checkpoints
	if ( m_bPreloadWordlist )
	{
		sphLogDebug ( "Prereading .spi" );
		if ( !PrereadSharedBuffer ( m_tWordlist.m_pBuf, "spi" ) )
			return false;
	}

	m_tProgress.Show ( true );

	//////////////////////
	// precalc everything
	//////////////////////

	// convert id32 to id64
	if ( m_dAttrShared.GetLengthBytes() && m_bId32to64 ) // FIXME!!! can't 32->64 convert ondisk_attrs
	{
		DWORD * pTarget = m_dAttrShared.GetWritePtr();
		const DWORD * pSource = pTarget + m_iDocinfo + 2 + m_iDocinfoIndex * 2;
		int iStride = m_tSchema.GetRowSize();
		SphDocID_t uDoc;
		int64_t iLimit = m_iDocinfo + ( ( m_uVersion < 20 ) ? 0 : m_iDocinfoIndex * 2 + 2 );
		for ( int64_t i=0; i<iLimit; i++ )
		{
			uDoc = *pSource; ///< wide id32 to id64
			DOCINFOSETID ( pTarget, uDoc );
			memcpy ( pTarget + DOCINFO_IDSIZE, pSource + 1, iStride * sizeof(DWORD) );
			pSource += iStride+1;
			pTarget += iStride+DOCINFO_IDSIZE;
		}
		sphWarning ( "id32 index loaded by id64 binary; attributes converted" );
	}

	// build attributes hash
	if ( m_tAttr.GetLengthBytes() && m_pDocinfoHash.GetLengthBytes() && !g_bDebugCheck )
	{
		sphLogDebug ( "Hashing docinfo" );
		assert ( CheckDocsCount ( m_iDocinfo, m_sLastError ) );
		int iStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
		SphDocID_t uFirst = DOCINFO2ID ( &m_tAttr[0] );
		SphDocID_t uRange = DOCINFO2ID ( &m_tAttr[ ( m_iDocinfo-1)*iStride ] ) - uFirst;
		DWORD iShift = 0;
		while ( uRange>=( 1 << DOCINFO_HASH_BITS ) )
		{
			iShift++;
			uRange >>= 1;
		}

		DWORD * pHash = m_pDocinfoHash.GetWritePtr();
		*pHash++ = iShift;
		*pHash = 0;
		DWORD uLastHash = 0;

		for ( int64_t i=1; i<m_iDocinfo; i++ )
		{
			assert ( DOCINFO2ID ( &m_tAttr[ i*iStride ] )>uFirst
				&& DOCINFO2ID ( &m_tAttr[ ( i-1 )*iStride ] ) < DOCINFO2ID ( &m_tAttr[ i*iStride ] )
				&& "descending document ID found" );
			DWORD uHash = (DWORD)( ( DOCINFO2ID ( &m_tAttr[ i*iStride ] ) - uFirst ) >> iShift );
			if ( uHash==uLastHash )
				continue;

			while ( uLastHash<uHash )
				pHash [ ++uLastHash ] = (DWORD)i;

			uLastHash = uHash;
		}
		pHash [ ++uLastHash ] = (DWORD)m_iDocinfo;
	}

	// persist MVA needs valid DocinfoHash
	sphLogDebug ( "Prereading .mvp" );
	if ( !LoadPersistentMVA ( m_sLastError ) )
		return false;

	// build "indexes" for full-scan
	if ( m_uVersion < 20 && !PrecomputeMinMax() )
		return false;

	// paranoid MVA verification
#if PARANOID
	// find out what attrs are MVA
	CSphVector<int> dMvaRowitem;
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tCol = m_tSchema.GetAttr(i);
		if ( tCol.m_eAttrType==SPH_ATTR_UINT32SET )
			dMvaRowitem.Add ( tCol.m_tLocator.m_iBitOffset/ROWITEM_BITS );
	}
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tCol = m_tSchema.GetAttr(i);
		if ( tCol.m_eAttrType==SPH_ATTR_INT64SET )
			dMvaRowitem.Add ( tCol.m_tLocator.m_iBitOffset/ROWITEM_BITS );
	}

	// for each docinfo entry, verify that MVA attrs point to right storage location
	int iStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
	for ( int64_t iDoc=0; iDoc<m_iDocinfo && dMvaRowitem.GetLength(); iDoc++ )
	{
		CSphRowitem * pRow = m_tAttr.GetWritePtr() + ( iDoc*iStride );
		CSphRowitem * pAttrs = DOCINFO2ATTRS(pRow);
		SphDocID_t uDocID = DOCINFO2ID(pRow);

		DWORD uOff = pAttrs[ dMvaRowitem[0] ];
		if ( !uOff )
		{
			// its either all or nothing
			ARRAY_FOREACH ( i, dMvaRowitem )
				assert ( pAttrs[ dMvaRowitem[i] ]==0 );
		} else if ( !( uOff & MVA_ARENA_FLAG ) )
		{
			assert ( uDocID==DOCINFO2ID ( m_tMva.GetWritePtr() + uOff - DOCINFO_IDSIZE ) );

			// walk the trail
			ARRAY_FOREACH ( i, dMvaRowitem )
			{
				assert ( pAttrs[ dMvaRowitem[i] ]==uOff );
				int iCount = m_tMva[uOff];
				uOff += 1+iCount;
			}
		}
	}
#endif // PARANOID

	*m_pPreread = 1;
	sphLogDebug ( "Preread successfully finished" );
	return true;
}


void CSphIndex_VLN::SetBase ( const char * sNewBase )
{
	m_sFilename = sNewBase;
}


bool CSphIndex_VLN::Rename ( const char * sNewBase )
{
	if ( m_sFilename==sNewBase )
		return true;

	// try to rename everything
	char sFrom [ SPH_MAX_FILENAME_LEN ];
	char sTo [ SPH_MAX_FILENAME_LEN ];

	// +1 for ".spl"
	int iExtCount = sphGetExtCount() + 1;
	const char ** sExts = sphGetExts ( SPH_EXT_TYPE_LOC );
	DWORD uMask = 0;

	int iExt;
	for ( iExt=0; iExt<iExtCount; iExt++ )
	{
		const char * sExt = sExts[iExt];
		if ( !strcmp ( sExt, ".spp" ) && m_uVersion<3 ) // .spp files are v3+
			continue;
		if ( !strcmp ( sExt, ".spm" ) && m_uVersion<4 ) // .spm files are v4+
			continue;
		if ( !strcmp ( sExt, ".spk" ) && m_uVersion<10 ) // .spk files are v10+
			continue;
		if ( !strcmp ( sExt, ".sps" ) && m_uVersion<17 ) // .sps files are v17+
			continue;
		if ( !strcmp ( sExt, ".spe" ) && m_uVersion<31 ) // .spe files are v31+
			continue;

#if !USE_WINDOWS
		if ( !strcmp ( sExt, ".spl" ) && m_iLockFD<0 ) // .spl files are locks
			continue;
#else
		if ( !strcmp ( sExt, ".spl" ) )
		{
			if ( m_iLockFD>=0 )
			{
				::close ( m_iLockFD );
				::unlink ( GetIndexFileName("spl").cstr() );
				sphLogDebug ( "lock %s unlinked, file with ID %d closed", GetIndexFileName("spl").cstr(), m_iLockFD );
				m_iLockFD = -1;
			}
			continue;
		}
#endif

		snprintf ( sFrom, sizeof(sFrom), "%s%s", m_sFilename.cstr(), sExt );
		snprintf ( sTo, sizeof(sTo), "%s%s", sNewBase, sExt );

#if USE_WINDOWS
		::unlink ( sTo );
		sphLogDebug ( "%s unlinked", sTo );
#endif

		if ( ::rename ( sFrom, sTo ) )
		{
			m_sLastError.SetSprintf ( "rename %s to %s failed: %s", sFrom, sTo, strerror(errno) );
			// this is no reason to fail if spl is missing, since it is only lock and no data.
			if ( strcmp ( sExt, ".spl" ) )
				break;
		}
		uMask |= ( 1UL << iExt );
	}

	// are we good?
	if ( iExt==iExtCount )
	{
		SetBase ( sNewBase );
		sphLogDebug ( "Base set to %s", sNewBase );
		return true;
	}

	// if there were errors, rollback
	for ( iExt=0; iExt<iExtCount; iExt++ )
	{
		if (!( uMask & ( 1UL << iExt ) ))
			continue;

		const char * sExt = sExts[iExt];
		snprintf ( sFrom, sizeof(sFrom), "%s%s", sNewBase, sExt );
		snprintf ( sTo, sizeof(sTo), "%s%s", m_sFilename.cstr(), sExt );
		if ( ::rename ( sFrom, sTo ) )
		{
			sphLogDebug ( "Rollback failure when renaming %s to %s", sFrom, sTo );
			// !COMMIT should handle rollback failures somehow
		}
	}
	return false;
}

//////////////////////////////////////////////////////////////////////////

CSphQueryContext::CSphQueryContext ()
{
	m_iWeights = 0;
	m_bLookupFilter = false;
	m_bLookupSort = false;
	m_bPackedFactors = false;
	m_pFilter = NULL;
	m_pWeightFilter = NULL;
	m_pIndexData = NULL;
	m_pProfile = NULL;
	m_pLocalDocs = NULL;
	m_iTotalDocs = 0;
}

CSphQueryContext::~CSphQueryContext ()
{
	SafeDelete ( m_pFilter );
	SafeDelete ( m_pWeightFilter );
}

void CSphQueryContext::BindWeights ( const CSphQuery * pQuery, const CSphSchema & tSchema )
{
	const int MIN_WEIGHT = 1;
	// const int HEAVY_FIELDS = 32;
	const int HEAVY_FIELDS = SPH_MAX_FIELDS;

	// defaults
	m_iWeights = Min ( tSchema.m_dFields.GetLength(), HEAVY_FIELDS );
	for ( int i=0; i<m_iWeights; i++ )
		m_dWeights[i] = MIN_WEIGHT;

	// name-bound weights
	if ( pQuery->m_dFieldWeights.GetLength() )
	{
		ARRAY_FOREACH ( i, pQuery->m_dFieldWeights )
		{
			int j = tSchema.GetFieldIndex ( pQuery->m_dFieldWeights[i].m_sName.cstr() );
			if ( j>=0 && j<HEAVY_FIELDS )
				m_dWeights[j] = Max ( MIN_WEIGHT, pQuery->m_dFieldWeights[i].m_iValue );
		}
		return;
	}

	// order-bound weights
	if ( pQuery->m_pWeights )
	{
		for ( int i=0; i<Min ( m_iWeights, pQuery->m_iWeights ); i++ )
			m_dWeights[i] = Max ( MIN_WEIGHT, (int)pQuery->m_pWeights[i] );
	}
}


bool CSphQueryContext::SetupCalc ( CSphQueryResult * pResult, const ISphSchema & tInSchema,
	const CSphSchema & tSchema, const DWORD * pMvaPool )
{
	m_dCalcFilter.Resize ( 0 );
	m_dCalcSort.Resize ( 0 );
	m_dCalcFinal.Resize ( 0 );

	// quickly verify that all my real attributes can be stashed there
	if ( tInSchema.GetAttrsCount() < tSchema.GetAttrsCount() )
	{
		pResult->m_sError.SetSprintf ( "INTERNAL ERROR: incoming-schema mismatch (incount=%d, mycount=%d)",
			tInSchema.GetAttrsCount(), tSchema.GetAttrsCount() );
		return false;
	}

	// now match everyone
	for ( int iIn=0; iIn<tInSchema.GetAttrsCount(); iIn++ )
	{
		const CSphColumnInfo & tIn = tInSchema.GetAttr(iIn);
		switch ( tIn.m_eStage )
		{
			case SPH_EVAL_STATIC:
			case SPH_EVAL_OVERRIDE:
			{
				// this check may significantly slow down queries with huge schema attribute count
#ifndef NDEBUG
				const CSphColumnInfo * pMy = tSchema.GetAttr ( tIn.m_sName.cstr() );
				if ( !pMy )
				{
					pResult->m_sError.SetSprintf ( "INTERNAL ERROR: incoming-schema attr missing from index-schema (in=%s)",
						sphDumpAttr(tIn).cstr() );
					return false;
				}

				if ( tIn.m_eStage==SPH_EVAL_OVERRIDE )
				{
					// override; check for type/size match and dynamic part
					if ( tIn.m_eAttrType!=pMy->m_eAttrType
						|| tIn.m_tLocator.m_iBitCount!=pMy->m_tLocator.m_iBitCount
						|| !tIn.m_tLocator.m_bDynamic )
					{
						pResult->m_sError.SetSprintf ( "INTERNAL ERROR: incoming-schema override mismatch (in=%s, my=%s)",
							sphDumpAttr(tIn).cstr(), sphDumpAttr(*pMy).cstr() );
						return false;
					}
				} else
				{
					// static; check for full match
					if (!( tIn==*pMy ))
					{
						pResult->m_sError.SetSprintf ( "INTERNAL ERROR: incoming-schema mismatch (in=%s, my=%s)",
							sphDumpAttr(tIn).cstr(), sphDumpAttr(*pMy).cstr() );
						return false;
					}
				}
#endif
				break;
			}

			case SPH_EVAL_PREFILTER:
			case SPH_EVAL_PRESORT:
			case SPH_EVAL_FINAL:
			{
				ISphExpr * pExpr = tIn.m_pExpr.Ptr();
				if ( !pExpr )
				{
					pResult->m_sError.SetSprintf ( "INTERNAL ERROR: incoming-schema expression missing evaluator (stage=%d, in=%s)",
						(int)tIn.m_eStage, sphDumpAttr(tIn).cstr() );
					return false;
				}

				// an expression that index/searcher should compute
				CalcItem_t tCalc;
				tCalc.m_eType = tIn.m_eAttrType;
				tCalc.m_tLoc = tIn.m_tLocator;
				tCalc.m_pExpr = pExpr;
				tCalc.m_pExpr->Command ( SPH_EXPR_SET_MVA_POOL, (void*)pMvaPool );

				switch ( tIn.m_eStage )
				{
					case SPH_EVAL_PREFILTER:	m_dCalcFilter.Add ( tCalc ); break;
					case SPH_EVAL_PRESORT:		m_dCalcSort.Add ( tCalc ); break;
					case SPH_EVAL_FINAL:		m_dCalcFinal.Add ( tCalc ); break;
					default:					break;
				}
				break;
			}

			case SPH_EVAL_SORTER:
				// sorter tells it will compute itself; so just skip it
			case SPH_EVAL_POSTLIMIT:
				break;

			default:
				pResult->m_sError.SetSprintf ( "INTERNAL ERROR: unhandled eval stage=%d", (int)tIn.m_eStage );
				return false;
		}
	}

	// ok, we can emit matches in this schema (incoming for sorter, outgoing for index/searcher)
	return true;
}


bool CSphIndex_VLN::IsStarDict () const
{
	return (
		( m_uVersion>=7 && ( m_tSettings.m_iMinPrefixLen>0 || m_tSettings.m_iMinInfixLen>0 ) && m_bEnableStar ) || // v.7 added mangling to infixes
		( m_uVersion==6 && ( m_tSettings.m_iMinPrefixLen>0 ) && m_bEnableStar ) ); // v.6 added mangling to prefixes
}


CSphDict * CSphIndex_VLN::SetupStarDict ( CSphScopedPtr<CSphDict> & tContainer, CSphDict * pPrevDict ) const
{
	// spawn wrapper, and put it in the box
	// wrapper type depends on version; v.8 introduced new mangling rules
	if ( !IsStarDict() )
		return pPrevDict;
	if ( m_uVersion>=8 )
		tContainer = new CSphDictStarV8 ( pPrevDict, m_tSettings.m_iMinPrefixLen>0, m_tSettings.m_iMinInfixLen>0 );
	else
		tContainer = new CSphDictStar ( pPrevDict );

	// FIXME? might wanna verify somehow that the tokenizer has '*' as a character
	return tContainer.Ptr();
}


CSphDict * CSphIndex_VLN::SetupExactDict ( CSphScopedPtr<CSphDict> & tContainer, CSphDict * pPrevDict ) const
{
	if ( m_uVersion<12 || !m_tSettings.m_bIndexExactWords )
		return pPrevDict;

	tContainer = new CSphDictExact ( pPrevDict );
	return tContainer.Ptr();
}


bool CSphIndex_VLN::GetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords,
	const char * szQuery, bool bGetStats, CSphString * pError ) const
{
	WITH_QWORD ( this, false, Qword, return DoGetKeywords<Qword> ( dKeywords, szQuery, bGetStats, false, pError ) );
	return false;
}


template < class Qword >
bool CSphIndex_VLN::DoGetKeywords ( CSphVector <CSphKeywordInfo> & dKeywords,
	const char * szQuery, bool bGetStats, bool bFillOnly, CSphString * pError ) const
{
	if ( !bFillOnly )
		dKeywords.Resize ( 0 );

	if ( !m_pPreread || !*m_pPreread )
	{
		if ( pError )
			*pError = "index not preread";
		return false;
	}

	// short-cut if no query or keywords to fill
	if ( ( bFillOnly && !dKeywords.GetLength() ) || ( !bFillOnly && ( !szQuery || !szQuery[0] ) ) )
		return true;

	CSphScopedPtr <CSphAutofile> pDoclist ( NULL );
	CSphScopedPtr <CSphAutofile> pHitlist ( NULL );

	// TODO: in case of bFillOnly skip tokenizer cloning and setup
	CSphScopedPtr<ISphTokenizer> pTokenizer ( m_pTokenizer->Clone ( SPH_CLONE_INDEX ) ); // avoid race
	pTokenizer->EnableTokenizedMultiformTracking ();

	// need to support '*' and '=' but not the other specials
	// so m_pQueryTokenizer does not work for us, gotta clone and setup one manually
	if ( IsStarDict() )
		pTokenizer->AddPlainChar ( '*' );
	if ( m_tSettings.m_bIndexExactWords )
		pTokenizer->AddPlainChar ( '=' );

	CSphScopedPtr<CSphDict> tDictCloned ( NULL );
	CSphDict * pDictBase = m_pDict;
	if ( pDictBase->HasState() )
		tDictCloned = pDictBase = pDictBase->Clone();

	CSphScopedPtr<CSphDict> tDict ( NULL );
	CSphDict * pDict = SetupStarDict ( tDict, pDictBase );

	CSphScopedPtr<CSphDict> tDict2 ( NULL );
	pDict = SetupExactDict ( tDict2, pDict );

	// FIXME!!! missed bigram, aot transform, FieldFilter

	// prepare for setup
	CSphAutofile tDummy1, tDummy2, tDummy3, tWordlist;

	if ( !m_bKeepFilesOpen )
	{
		CSphString sTmp;
		if ( tWordlist.Open ( GetIndexFileName ( "spi" ), SPH_O_READ, ( pError ? *pError : sTmp ) ) < 0 )
			return false;
	}

	DiskIndexQwordSetup_c tTermSetup ( tDummy1, tDummy2
		, m_bPreloadWordlist ? tDummy3 : ( m_bKeepFilesOpen ? m_tWordlist.m_tFile : tWordlist )
		, m_bPreloadWordlist ? 0 : m_tWordlist.m_iMaxChunk, m_pSkiplists.GetWritePtr(), NULL );
	tTermSetup.m_pDict = pDict;
	tTermSetup.m_pIndex = this;
	tTermSetup.m_eDocinfo = m_tSettings.m_eDocinfo;

	Qword QueryWord ( false, false );

	if ( !bFillOnly )
	{
		CSphString sTokenized;
		BYTE * sWord;
		pTokenizer->SetBuffer ( (const BYTE *)szQuery, strlen(szQuery) );

		// FIXME!!! got rid of duplicated term stat and qword setup
		while ( ( sWord = pTokenizer->GetToken() )!=NULL )
		{
			BYTE * sMultiform = pTokenizer->GetTokenizedMultiform();
			if ( sMultiform )
				sTokenized = (const char*)sMultiform;
			else
				sTokenized = (const char*)sWord;

			SphWordID_t iWord = pDict->GetWordID ( sWord );
			if ( iWord )
			{
				if ( bGetStats )
				{
					QueryWord.Reset ();
					QueryWord.m_sWord = (const char*)sWord;
					QueryWord.m_sDictWord = (const char*)sWord;
					QueryWord.m_iWordID = iWord;
					tTermSetup.QwordSetup ( &QueryWord );
				}

				CSphKeywordInfo & tInfo = dKeywords.Add();
				Swap ( tInfo.m_sTokenized, sTokenized );
				tInfo.m_sNormalized = (const char*)sWord;
				tInfo.m_iDocs = bGetStats ? QueryWord.m_iDocs : 0;
				tInfo.m_iHits = bGetStats ? QueryWord.m_iHits : 0;

				if ( tInfo.m_sNormalized.cstr()[0]==MAGIC_WORD_HEAD_NONSTEMMED )
					*(char *)tInfo.m_sNormalized.cstr() = '=';
			}
		}
	} else
	{
		BYTE sWord[MAX_KEYWORD_BYTES];

		ARRAY_FOREACH ( i, dKeywords )
		{
			CSphKeywordInfo & tInfo = dKeywords[i];
			int iLen = tInfo.m_sTokenized.Length();
			memcpy ( sWord, tInfo.m_sTokenized.cstr(), iLen );
			sWord[iLen] = '\0';

			SphWordID_t iWord = pDict->GetWordID ( sWord );
			if ( iWord )
			{
				QueryWord.Reset ();
				QueryWord.m_sWord = tInfo.m_sTokenized;
				QueryWord.m_sDictWord = (const char*)sWord;
				QueryWord.m_iWordID = iWord;
				tTermSetup.QwordSetup ( &QueryWord );

				tInfo.m_iDocs += QueryWord.m_iDocs;
				tInfo.m_iHits += QueryWord.m_iHits;
			}
		}
	}

	return true;
}


bool CSphIndex_VLN::FillKeywords ( CSphVector <CSphKeywordInfo> & dKeywords ) const
{
	WITH_QWORD ( this, false, Qword, return DoGetKeywords<Qword> ( dKeywords, NULL, true, true, NULL ) );
	return false;
}


// fix MSVC 2005 fuckup, template DoGetKeywords() just above somehow resets forScope
#if USE_WINDOWS
#pragma conform(forScope,on)
#endif


static bool IsWeightColumn ( const CSphString & sAttr, const ISphSchema & tSchema )
{
	if ( sAttr=="@weight" )
		return true;

	const CSphColumnInfo * pCol = tSchema.GetAttr ( sAttr.cstr() );
	return ( pCol && pCol->m_bWeight );
}


bool CSphQueryContext::CreateFilters ( bool bFullscan,
	const CSphVector<CSphFilterSettings> * pdFilters, const ISphSchema & tSchema,
	const DWORD * pMvaPool, const BYTE * pStrings, CSphString & sError )
{
	if ( !pdFilters )
		return true;
	ARRAY_FOREACH ( i, (*pdFilters) )
	{
		const CSphFilterSettings & tFilter = (*pdFilters)[i];
		if ( tFilter.m_sAttrName.IsEmpty() )
			continue;

		bool bWeight = IsWeightColumn ( tFilter.m_sAttrName, tSchema );

		if ( bFullscan && bWeight )
			continue; // @weight is not avaiable in fullscan mode

		ISphFilter * pFilter = sphCreateFilter ( tFilter, tSchema, pMvaPool, pStrings, sError );
		if ( !pFilter )
			return false;

		ISphFilter ** pGroup = bWeight ? &m_pWeightFilter : &m_pFilter;
		*pGroup = sphJoinFilters ( *pGroup, pFilter );
	}
	if ( m_pFilter )
		m_pFilter = m_pFilter->Optimize();
	return true;
}


bool CSphQueryContext::SetupOverrides ( const CSphQuery * pQuery, CSphQueryResult * pResult, const CSphSchema & tIndexSchema, const ISphSchema & tOutgoingSchema )
{
	m_pOverrides = NULL;
	m_dOverrideIn.Resize ( pQuery->m_dOverrides.GetLength() );
	m_dOverrideOut.Resize ( pQuery->m_dOverrides.GetLength() );

	ARRAY_FOREACH ( i, pQuery->m_dOverrides )
	{
		const char * sAttr = pQuery->m_dOverrides[i].m_sAttr.cstr(); // shortcut
		const CSphColumnInfo * pCol = tIndexSchema.GetAttr ( sAttr );
		if ( !pCol )
		{
			pResult->m_sError.SetSprintf ( "attribute override: unknown attribute name '%s'", sAttr );
			return false;
		}

		if ( pCol->m_eAttrType!=pQuery->m_dOverrides[i].m_eAttrType )
		{
			pResult->m_sError.SetSprintf ( "attribute override: attribute '%s' type mismatch (index=%d, query=%d)",
				sAttr, pCol->m_eAttrType, pQuery->m_dOverrides[i].m_eAttrType );
			return false;
		}

		const CSphColumnInfo * pOutCol = tOutgoingSchema.GetAttr ( pQuery->m_dOverrides[i].m_sAttr.cstr() );
		if ( !pOutCol )
		{
			pResult->m_sError.SetSprintf ( "attribute override: unknown attribute name '%s' in outgoing schema", sAttr );
			return false;
		}

		m_dOverrideIn[i] = pCol->m_tLocator;
		m_dOverrideOut[i] = pOutCol->m_tLocator;

#ifndef NDEBUG
		// check that the values are actually sorted
		const CSphVector<CSphAttrOverride::IdValuePair_t> & dValues = pQuery->m_dOverrides[i].m_dValues;
		for ( int j=1; j<dValues.GetLength(); j++ )
			assert ( dValues[j-1] < dValues[j] );
#endif
	}

	if ( pQuery->m_dOverrides.GetLength() )
		m_pOverrides = &pQuery->m_dOverrides;
	return true;
}

static int sphQueryHeightCalc ( const XQNode_t * pNode )
{
	if ( !pNode->m_dChildren.GetLength() )
	{
		// exception, pre-cached OR of tiny (rare) keywords is just one node
		if ( pNode->GetOp()==SPH_QUERY_OR )
		{
#ifndef NDEBUG
			// sanity checks
			// this node must be only created for a huge OR of tiny expansions
			assert ( pNode->m_dWords.GetLength() );
			ARRAY_FOREACH ( i, pNode->m_dWords )
			{
				assert ( pNode->m_dWords[i].m_iAtomPos==pNode->m_dWords[0].m_iAtomPos );
				assert ( pNode->m_dWords[i].m_bExpanded );
			}
#endif
			return 1;
		}
		return pNode->m_dWords.GetLength();
	}

	if ( pNode->GetOp()==SPH_QUERY_BEFORE )
		return 1;

	int iMaxChild = 0;
	int iHeight = 0;
	ARRAY_FOREACH ( i, pNode->m_dChildren )
	{
		int iBottom = sphQueryHeightCalc ( pNode->m_dChildren[i] );
		int iTop = pNode->m_dChildren.GetLength()-i-1;
		if ( iBottom+iTop>=iMaxChild+iHeight )
		{
			iMaxChild = iBottom;
			iHeight = iTop;
		}
	}

	return iMaxChild+iHeight;
}

#define SPH_EXTNODE_STACK_SIZE 160

bool sphCheckQueryHeight ( const XQNode_t * pRoot, CSphString & sError )
{
	int iHeight = 0;
	if ( pRoot )
		iHeight = sphQueryHeightCalc ( pRoot );

	int64_t iQueryStack = sphGetStackUsed() + iHeight*SPH_EXTNODE_STACK_SIZE;
	bool bValid = ( g_iThreadStackSize>=iQueryStack );
	if ( !bValid )
		sError.SetSprintf ( "query too complex, not enough stack (thread_stack=%dK or higher required)",
			(int)( ( iQueryStack + 1024 - ( iQueryStack%1024 ) ) / 1024 ) );
	return bValid;
}

static XQNode_t * CloneKeyword ( const XQNode_t * pNode )
{
	assert ( pNode );

	XQNode_t * pRes = new XQNode_t ( pNode->m_dSpec );
	pRes->m_dWords = pNode->m_dWords;
	return pRes;
}


static XQNode_t * ExpandKeyword ( XQNode_t * pNode, const CSphIndexSettings & tSettings, bool bStarEnabled )
{
	assert ( pNode );

	XQNode_t * pExpand = new XQNode_t ( pNode->m_dSpec );
	pExpand->SetOp ( SPH_QUERY_OR, pNode );

	if ( tSettings.m_iMinInfixLen>0 && bStarEnabled )
	{
		assert ( pNode->m_dChildren.GetLength()==0 );
		assert ( pNode->m_dWords.GetLength()==1 );
		XQNode_t * pInfix = CloneKeyword ( pNode );
		pInfix->m_dWords[0].m_sWord.SetSprintf ( "*%s*", pNode->m_dWords[0].m_sWord.cstr() );
		pInfix->m_pParent = pExpand;
		pExpand->m_dChildren.Add ( pInfix );
	} else if ( tSettings.m_iMinPrefixLen>0 )
	{
		assert ( pNode->m_dChildren.GetLength()==0 );
		assert ( pNode->m_dWords.GetLength()==1 );
		XQNode_t * pPrefix = CloneKeyword ( pNode );
		pPrefix->m_dWords[0].m_sWord.SetSprintf ( "%s*", pNode->m_dWords[0].m_sWord.cstr() );
		pPrefix->m_pParent = pExpand;
		pExpand->m_dChildren.Add ( pPrefix );
	}

	if ( tSettings.m_bIndexExactWords )
	{
		assert ( pNode->m_dChildren.GetLength()==0 );
		assert ( pNode->m_dWords.GetLength()==1 );
		XQNode_t * pExact = CloneKeyword ( pNode );
		pExact->m_dWords[0].m_sWord.SetSprintf ( "=%s", pNode->m_dWords[0].m_sWord.cstr() );
		pExact->m_pParent = pExpand;
		pExpand->m_dChildren.Add ( pExact );
	}

	return pExpand;
}

XQNode_t * sphQueryExpandKeywords ( XQNode_t * pNode, const CSphIndexSettings & tSettings, bool bStarEnabled )
{
	// only if expansion makes sense at all
	if ( tSettings.m_iMinInfixLen<=0 && tSettings.m_iMinPrefixLen<=0 && !tSettings.m_bIndexExactWords )
		return pNode;

	// process children for composite nodes
	if ( pNode->m_dChildren.GetLength() )
	{
		ARRAY_FOREACH ( i, pNode->m_dChildren )
		{
			pNode->m_dChildren[i] = sphQueryExpandKeywords ( pNode->m_dChildren[i], tSettings, bStarEnabled );
			pNode->m_dChildren[i]->m_pParent = pNode;
		}
		return pNode;
	}

	// if that's a phrase/proximity node, create a very special, magic phrase/proximity node
	if ( pNode->GetOp()==SPH_QUERY_PHRASE || pNode->GetOp()==SPH_QUERY_PROXIMITY || pNode->GetOp()==SPH_QUERY_QUORUM )
	{
		assert ( pNode->m_dWords.GetLength()>1 );
		ARRAY_FOREACH ( i, pNode->m_dWords )
		{
			XQNode_t * pWord = new XQNode_t ( pNode->m_dSpec );
			pWord->m_dWords.Add ( pNode->m_dWords[i] );
			pNode->m_dChildren.Add ( ExpandKeyword ( pWord, tSettings, bStarEnabled ) );
			pNode->m_dChildren.Last()->m_iAtomPos = pNode->m_dWords[i].m_iAtomPos;
			pNode->m_dChildren.Last()->m_pParent = pNode;
		}
		pNode->m_dWords.Reset();
		pNode->m_bVirtuallyPlain = true;
		return pNode;
	}

	// skip empty plain nodes
	if ( pNode->m_dWords.GetLength()<=0 )
		return pNode;

	// process keywords for plain nodes
	assert ( pNode->m_dWords.GetLength()==1 );

	XQKeyword_t & tKeyword = pNode->m_dWords[0];
	if ( tKeyword.m_sWord.Begins("=")
		|| tKeyword.m_sWord.Begins("*")
		|| tKeyword.m_sWord.Ends("*") )
	{
		return pNode;
	}

	// do the expansion
	return ExpandKeyword ( pNode, tSettings, bStarEnabled );
}


void sphQueryAdjustStars ( XQNode_t * pNode, const CSphIndexSettings & tSettings )
{
	if ( pNode->m_dChildren.GetLength() )
	{
		ARRAY_FOREACH ( i, pNode->m_dChildren )
			sphQueryAdjustStars ( pNode->m_dChildren[i], tSettings );
		return;
	}

	ARRAY_FOREACH ( i, pNode->m_dWords )
	{
		CSphString & sWord = pNode->m_dWords[i].m_sWord;

		// trim all wildcards
		const char * s = sWord.cstr();
		int iLen = sWord.Length();
		while ( iLen>0 && sphIsWild ( s[iLen-1] ) )
			iLen--;
		while ( iLen>0 && sphIsWild(*s) )
		{
			s++;
			iLen--;
		}
		sWord = sWord.SubString ( (int)( s-sWord.cstr() ), iLen );

		// and now append stars if needed
		if ( tSettings.m_iMinPrefixLen>0 && iLen>=tSettings.m_iMinPrefixLen )
			sWord = sWord.SetSprintf ( "%s*", sWord.cstr() );
		else if ( tSettings.m_iMinInfixLen>0 && iLen>=tSettings.m_iMinInfixLen )
			sWord = sWord.SetSprintf ( "*%s*", sWord.cstr() );
	}
}


// transform the "one two three"/1 quorum into one|two|three (~40% faster)
static void TransformQuorum ( XQNode_t ** ppNode )
{
	XQNode_t *& pNode = *ppNode;
	if ( pNode->GetOp()!=SPH_QUERY_QUORUM || pNode->m_iOpArg!=1 )
		return;

	assert ( pNode->m_dChildren.GetLength()==0 );
	CSphVector<XQNode_t*> dArgs;
	ARRAY_FOREACH ( i, pNode->m_dWords )
	{
		XQNode_t * pAnd = new XQNode_t ( pNode->m_dSpec );
		pAnd->m_dWords.Add ( pNode->m_dWords[i] );
		dArgs.Add ( pAnd );
	}
	pNode->m_dWords.Reset();
	pNode->SetOp ( SPH_QUERY_OR, dArgs );
}


struct BinaryNode_t
{
	int m_iLo;
	int m_iHi;
};

static void BuildExpandedTree ( const XQKeyword_t & tRootWord, CSphVector<CSphNamedInt> & dWordSrc, XQNode_t * pRoot, bool bMergeSingles )
{
	assert ( dWordSrc.GetLength() );
	pRoot->m_dWords.Reset();

	// put all tiny enough expansions in a single node
	int iTinyStart = 0;
	if ( pRoot->m_dSpec.m_dZones.GetLength() || !bMergeSingles )
	{
		// OPTIMIZE
		// ExtCached_c only supports field filtering but not zone filtering for now
		// so we skip tiny expansions optimizations in that case; we also do that in RT case
		// FIXME!!! why not in RT case??? check that case and perf
		iTinyStart = dWordSrc.GetLength();
	} else
	{
		// lookup where those start, relying on that dWordSrc should be reverse sorted
		while ( iTinyStart<dWordSrc.GetLength() && dWordSrc[iTinyStart].m_iValue>1 )
			iTinyStart++;
	}

	XQNode_t * pTiny = NULL;
	if ( iTinyStart!=dWordSrc.GetLength() )
	{
		if ( iTinyStart==0 )
			pTiny = pRoot;
		else
			pTiny = new XQNode_t ( pRoot->m_dSpec );

		pTiny->SetOp ( SPH_QUERY_OR );
		for ( int i=iTinyStart; i<dWordSrc.GetLength(); i++ )
		{
			XQKeyword_t & tWord = pTiny->m_dWords.Add();
			tWord.m_sWord = dWordSrc[i].m_sName;
			tWord.m_iAtomPos = tRootWord.m_iAtomPos;
			tWord.m_bExpanded = true;
			tWord.m_bFieldStart = tRootWord.m_bFieldStart;
			tWord.m_bFieldEnd = tRootWord.m_bFieldEnd;
		}

		// if we created a new node, we have to propagate field/zone specs there
		if ( pTiny!=pRoot )
			pTiny->CopySpecs ( pRoot );

		if ( iTinyStart==0 )
			return;
		dWordSrc.Resize ( iTinyStart );
	}

	// build a binary tree from all the other expansions
	CSphVector<BinaryNode_t> dNodes;
	dNodes.Reserve ( dWordSrc.GetLength() );

	XQNode_t * pCur = pRoot;

	dNodes.Add();
	dNodes.Last().m_iLo = 0;
	dNodes.Last().m_iHi = ( dWordSrc.GetLength()-1 );

	while ( dNodes.GetLength() )
	{
		BinaryNode_t tNode = dNodes.Pop();
		if ( tNode.m_iHi<tNode.m_iLo )
		{
			pCur = pCur->m_pParent;
			continue;
		}

		int iMid = ( tNode.m_iLo+tNode.m_iHi ) / 2;
		dNodes.Add ();
		dNodes.Last().m_iLo = tNode.m_iLo;
		dNodes.Last().m_iHi = iMid-1;
		dNodes.Add ();
		dNodes.Last().m_iLo = iMid+1;
		dNodes.Last().m_iHi = tNode.m_iHi;

		if ( pCur->m_dWords.GetLength() )
		{
			assert ( pCur->m_dWords.GetLength()==1 );
			XQNode_t * pTerm = CloneKeyword ( pRoot );
			Swap ( pTerm->m_dWords, pCur->m_dWords );
			pCur->m_dChildren.Add ( pTerm );
			pTerm->m_pParent = pCur;
		}

		XQNode_t * pChild = CloneKeyword ( pRoot );
		pChild->m_dWords.Add ( tRootWord );
		pChild->m_dWords.Last().m_sWord.Swap ( dWordSrc[iMid].m_sName );
		pChild->m_dWords.Last().m_bExpanded = true;
		pChild->m_bNotWeighted = ( dWordSrc[iMid].m_iValue==0 );

		pChild->m_pParent = pCur;
		pCur->m_dChildren.Add ( pChild );
		pCur->SetOp ( SPH_QUERY_OR );

		pCur = pChild;
	}

	if ( pTiny )
	{
		assert ( pRoot->GetOp()==SPH_QUERY_OR );
		assert ( pRoot->m_dChildren.GetLength() );
		assert ( pRoot!=pTiny );
		pRoot->m_dChildren.Add ( pTiny );
		pTiny->m_pParent = pRoot;
	}
}

void Swap ( CSphNamedInt & a, CSphNamedInt & b )
{
	a.m_sName.Swap ( b.m_sName );
	Swap ( a.m_iValue, b.m_iValue );
}

struct WordDocsGreaterOp_t
{
	inline bool IsLess ( const CSphNamedInt & a, const CSphNamedInt & b )
	{
		return a.m_iValue > b.m_iValue;
	}
};


/// do wildcard expansion for keywords dictionary
/// (including prefix and infix expansion)
XQNode_t * sphExpandXQNode ( XQNode_t * pNode, ExpansionContext_t & tCtx )
{
	assert ( pNode );
	assert ( tCtx.m_pResult );

	// process children for composite nodes
	if ( pNode->m_dChildren.GetLength() )
	{
		ARRAY_FOREACH ( i, pNode->m_dChildren )
		{
			pNode->m_dChildren[i] = sphExpandXQNode ( pNode->m_dChildren[i], tCtx );
			pNode->m_dChildren[i]->m_pParent = pNode;
		}
		return pNode;
	}

	// if that's a phrase/proximity node, create a very special, magic phrase/proximity node
	if ( pNode->GetOp()==SPH_QUERY_PHRASE || pNode->GetOp()==SPH_QUERY_PROXIMITY || pNode->GetOp()==SPH_QUERY_QUORUM )
	{
		assert ( pNode->m_dWords.GetLength()>1 );
		ARRAY_FOREACH ( i, pNode->m_dWords )
		{
			XQNode_t * pWord = new XQNode_t ( pNode->m_dSpec );
			pWord->m_dWords.Add ( pNode->m_dWords[i] );
			pNode->m_dChildren.Add ( sphExpandXQNode ( pWord, tCtx ) );
			pNode->m_dChildren.Last()->m_iAtomPos = pNode->m_dWords[i].m_iAtomPos;
			pNode->m_dChildren.Last()->m_pParent = pNode;

			// tricky part
			// current node may have field/zone limits attached
			// normally those get pushed down during query parsing
			// but here we create nodes manually and have to push down limits too
			pWord->CopySpecs ( pNode );
		}
		pNode->m_dWords.Reset();
		pNode->m_bVirtuallyPlain = true;
		return pNode;
	}

	// skip empty plain nodes
	if ( pNode->m_dWords.GetLength()<=0 )
		return pNode;

	// process keywords for plain nodes
	assert ( pNode->m_dChildren.GetLength()==0 );
	assert ( pNode->m_dWords.GetLength()==1 );

	// check the wildcards
	const char * sFull = pNode->m_dWords[0].m_sWord.cstr();
	const int iLen = strlen ( sFull );

	int iWilds = 0;
	for ( const char * s = sFull; *s; s++ )
		if ( sphIsWild(*s) )
			iWilds++;

	// no wildcards, or just wildcards? do not expand
	if ( !iWilds || iWilds==iLen )
		return pNode;

	CSphVector<CSphNamedInt> dExpanded;
	if ( !sphIsWild(*sFull) || tCtx.m_iMinInfixLen==0 )
	{
		// do prefix expansion
		// remove exact form modifier, if any
		const char * sPrefix = sFull;
		if ( *sPrefix=='=' )
			sPrefix++;

		// skip leading wildcards
		// (in case we got here on non-infixed index path)
		const char * sWildcard = sPrefix;
		while ( sphIsWild ( *sPrefix ) )
		{
			sPrefix++;
			sWildcard++;
		}

		// compute non-wildcard prefix length
		int iPrefix = 0;
		for ( const char * s = sPrefix; *s && !sphIsWild(*s); s++ )
			iPrefix++;

		// do not expand prefixes under min length
		int iMinLen = Max ( tCtx.m_iMinPrefixLen, tCtx.m_iMinInfixLen );
		if ( iPrefix<iMinLen )
			return pNode;

		// prefix expansion should work on nonstemmed words only
		char sFixed [ MAX_KEYWORD_BYTES ];
		if ( tCtx.m_bHasMorphology )
		{
			sFixed[0] = MAGIC_WORD_HEAD_NONSTEMMED;
			memcpy ( sFixed+1, sPrefix, iPrefix );
			sPrefix = sFixed;
			iPrefix++;
		}

		tCtx.m_pWordlist->GetPrefixedWords ( sPrefix, iPrefix, sWildcard, dExpanded, tCtx.m_pBuf, tCtx.m_iFD );

	} else
	{
		// do infix expansion
		assert ( sphIsWild(*sFull) );
		assert ( tCtx.m_iMinInfixLen>0 );

		// find the longest substring of non-wildcards
		const char * sMaxInfix = NULL;
		int iMaxInfix = 0;
		int iCur = 0;

		for ( const char * s = sFull; *s; s++ )
		{
			if ( sphIsWild(*s) )
			{
				iCur = 0;
			} else if ( ++iCur > iMaxInfix )
			{
				sMaxInfix = s-iCur+1;
				iMaxInfix = iCur;
			}
		}

		// do not expand infixes under min_infix_len
		if ( iMaxInfix < tCtx.m_iMinInfixLen )
			return pNode;

		// ignore heading star
		tCtx.m_pWordlist->GetInfixedWords ( sMaxInfix, iMaxInfix, sFull, dExpanded );
	}

	// no real expansions?
	// mark source word as expanded to prevent warning on terms mismatch in statistics
	if ( !dExpanded.GetLength() )
	{
		pNode->m_dWords.Begin()->m_bExpanded = true;
		return pNode;
	}

	// sort expansions by frequency desc
	// clip the less frequent ones if needed, as they are likely misspellings
	dExpanded.Sort ( WordDocsGreaterOp_t() );
	if ( tCtx.m_iExpansionLimit && tCtx.m_iExpansionLimit<dExpanded.GetLength() )
		dExpanded.Resize ( tCtx.m_iExpansionLimit );

	// mark new words as expanded to skip theirs check on merge
	// (expanded words differ across indexes)
	ARRAY_FOREACH ( i, dExpanded )
		tCtx.m_pResult->AddStat ( dExpanded[i].m_sName, 0, 0, true );

	// replace MAGIC_WORD_HEAD_NONSTEMMED symbol to '='
	if ( tCtx.m_bHasMorphology )
		ARRAY_FOREACH ( i, dExpanded )
			if ( dExpanded[i].m_sName.cstr()[0]==MAGIC_WORD_HEAD_NONSTEMMED )
				( (char *)dExpanded[i].m_sName.cstr() )[0] = '=';

	// copy the original word (iirc it might get overwritten),
	// and build a binary tree of all the expansions
	const XQKeyword_t tRootWord = pNode->m_dWords[0];
	BuildExpandedTree ( tRootWord, dExpanded, pNode, tCtx.m_bMergeSingles );

	return pNode;
}

XQNode_t * CSphIndex_VLN::ExpandPrefix ( XQNode_t * pNode, CSphString & sError, CSphQueryResultMeta * pResult ) const
{
	if ( !pNode || !m_pDict->GetSettings().m_bWordDict || ( m_tSettings.m_iMinPrefixLen<=0 && m_tSettings.m_iMinInfixLen<=0 ) )
		return pNode;

	// thread safe outer storage for dictionaries chunks and file
	BYTE * pBuf = NULL;
	int iFD = -1;
	CSphAutofile rdWordlist;
	if ( !m_bPreloadWordlist )
	{
		if ( m_bKeepFilesOpen )
			iFD = m_tWordlist.m_tFile.GetFD();
		else
		{
			iFD = rdWordlist.Open ( GetIndexFileName ( "spi" ), SPH_O_READ, sError );
			if ( iFD<0 )
				return NULL;
		}

		if ( m_tWordlist.m_iMaxChunk>0 )
			pBuf = new BYTE [ m_tWordlist.m_iMaxChunk ];
	}

	assert ( m_pPreread && *m_pPreread );
	assert ( !m_bPreloadWordlist || !m_tWordlist.m_pBuf.IsEmpty() );

	ExpansionContext_t tCtx;
	tCtx.m_pWordlist = &m_tWordlist;
	tCtx.m_pBuf = pBuf;
	tCtx.m_pResult = pResult;
	tCtx.m_iFD = iFD;
	tCtx.m_iMinPrefixLen = m_tSettings.m_iMinPrefixLen;
	tCtx.m_iMinInfixLen = m_tSettings.m_iMinInfixLen;
	tCtx.m_iExpansionLimit = m_iExpansionLimit;
	tCtx.m_bHasMorphology = m_pDict->HasMorphology();
	tCtx.m_bMergeSingles = ( m_tSettings.m_eDocinfo!=SPH_DOCINFO_INLINE );

	pNode = sphExpandXQNode ( pNode, tCtx );
	pNode->Check ( true );
	SafeDeleteArray ( pBuf );

	return pNode;
}


// transform the (A B) NEAR C into A NEAR B NEAR C
static void TransformNear ( XQNode_t ** ppNode )
{
	XQNode_t *& pNode = *ppNode;
	if ( pNode->GetOp()==SPH_QUERY_NEAR )
	{
		assert ( pNode->m_dWords.GetLength()==0 );
		CSphVector<XQNode_t*> dArgs;
		int iStartFrom;

		// transform all (A B C) NEAR D into A NEAR B NEAR C NEAR D
		do
		{
			dArgs.Reset();
			iStartFrom = 0;
			ARRAY_FOREACH ( i, pNode->m_dChildren )
			{
				XQNode_t * pChild = pNode->m_dChildren[i]; ///< shortcut
				if ( pChild->GetOp()==SPH_QUERY_AND && pChild->m_dChildren.GetLength()>0 )
				{
					ARRAY_FOREACH ( j, pChild->m_dChildren )
					{
						if ( j==0 && iStartFrom==0 )
						{
							// we will remove the node anyway, so just replace it with 1-st child instead
							pNode->m_dChildren[i] = pChild->m_dChildren[j];
							pNode->m_dChildren[i]->m_pParent = pNode;
							iStartFrom = i+1;
						} else
						{
							dArgs.Add ( pChild->m_dChildren[j] );
						}
					}
					pChild->m_dChildren.Reset();
					SafeDelete ( pChild );
				} else if ( iStartFrom!=0 )
				{
					dArgs.Add ( pChild );
				}
			}

			if ( iStartFrom!=0 )
			{
				pNode->m_dChildren.Resize ( iStartFrom + dArgs.GetLength() );
				ARRAY_FOREACH ( i, dArgs )
				{
					pNode->m_dChildren [ i + iStartFrom ] = dArgs[i];
					pNode->m_dChildren [ i + iStartFrom ]->m_pParent = pNode;
				}
			}
		} while ( iStartFrom!=0 );
	}

	ARRAY_FOREACH ( i, pNode->m_dChildren )
		TransformNear ( &pNode->m_dChildren[i] );
}


/// tag excluded keywords (rvals to operator NOT)
static void TagExcluded ( XQNode_t * pNode, bool bNot )
{
	if ( pNode->GetOp()==SPH_QUERY_ANDNOT )
	{
		assert ( pNode->m_dChildren.GetLength()==2 );
		assert ( pNode->m_dWords.GetLength()==0 );
		TagExcluded ( pNode->m_dChildren[0], bNot );
		TagExcluded ( pNode->m_dChildren[1], !bNot );

	} else if ( pNode->m_dChildren.GetLength() )
	{
		// FIXME? check if this works okay with "virtually plain" stuff?
		ARRAY_FOREACH ( i, pNode->m_dChildren )
			TagExcluded ( pNode->m_dChildren[i], bNot );
	} else
	{
		// tricky bit
		// no assert on length here and that is intended
		// we have fully empty nodes (0 children, 0 words) sometimes!
		ARRAY_FOREACH ( i, pNode->m_dWords )
			pNode->m_dWords[i].m_bExcluded = bNot;
	}
}


/// optimize phrase queries if we have bigrams
static void TransformBigrams ( XQNode_t * pNode, const CSphIndexSettings & tSettings )
{
	assert ( tSettings.m_eBigramIndex!=SPH_BIGRAM_NONE );
	assert ( tSettings.m_eBigramIndex==SPH_BIGRAM_ALL || tSettings.m_dBigramWords.GetLength() );

	if ( pNode->GetOp()!=SPH_QUERY_PHRASE )
	{
		ARRAY_FOREACH ( i, pNode->m_dChildren )
			TransformBigrams ( pNode->m_dChildren[i], tSettings );
		return;
	}

	CSphBitvec bmRemove;
	bmRemove.Init ( pNode->m_dWords.GetLength() );

	for ( int i=0; i<pNode->m_dWords.GetLength()-1; i++ )
	{
		// check whether this pair was indexed
		bool bBigram = false;
		switch ( tSettings.m_eBigramIndex )
		{
			case SPH_BIGRAM_NONE:
				break;
			case SPH_BIGRAM_ALL:
				bBigram = true;
				break;
			case SPH_BIGRAM_FIRSTFREQ:
				bBigram = tSettings.m_dBigramWords.BinarySearch ( pNode->m_dWords[i].m_sWord )!=NULL;
				break;
			case SPH_BIGRAM_BOTHFREQ:
				bBigram =
					( tSettings.m_dBigramWords.BinarySearch ( pNode->m_dWords[i].m_sWord )!=NULL ) &&
					( tSettings.m_dBigramWords.BinarySearch ( pNode->m_dWords[i+1].m_sWord )!=NULL );
				break;
		}
		if ( !bBigram )
			continue;

		// replace the pair with a bigram keyword
		// FIXME!!! set phrase weight for this "word" here
		pNode->m_dWords[i].m_sWord.SetSprintf ( "%s%c%s",
			pNode->m_dWords[i].m_sWord.cstr(),
			MAGIC_WORD_BIGRAM,
			pNode->m_dWords[i+1].m_sWord.cstr() );

		// only mark for removal now, we will sweep later
		// so that [a b c] would convert to ["a b" "b c"], not just ["a b" c]
		bmRemove.BitClear ( i );
		bmRemove.BitSet ( i+1 );
	}

	// remove marked words
	int iOut = 0;
	ARRAY_FOREACH ( i, pNode->m_dWords )
		if ( !bmRemove.BitGet(i) )
			pNode->m_dWords[iOut++] = pNode->m_dWords[i];
	pNode->m_dWords.Resize ( iOut );

	// fixup nodes that are not real phrases any more
	if ( pNode->m_dWords.GetLength()==1 )
		pNode->SetOp ( SPH_QUERY_AND );
}


/// create a node from a set of lemmas
/// WARNING, tKeyword might or might not be pointing to pNode->m_dWords[0]
/// Called from the daemon side (searchd) in time of query
static void TransformAotFilterKeyword ( XQNode_t * pNode, const XQKeyword_t & tKeyword, bool bUtf8, const CSphWordforms * pWordforms, const CSphIndexSettings & tSettings )
{
	assert ( pNode->m_dWords.GetLength()<=1 );
	assert ( pNode->m_dChildren.GetLength()==0 );

	XQNode_t * pExact = NULL;
	if ( pWordforms )
	{
		// do a copy, because patching in place is not an option
		// short => longlonglong wordform mapping would crash
		// OPTIMIZE? forms that are not found will (?) get looked up again in the dict
		char sBuf [ MAX_KEYWORD_BYTES ];
		strncpy ( sBuf, tKeyword.m_sWord.cstr(), sizeof(sBuf) );
		if ( pWordforms->ToNormalForm ( (BYTE*)sBuf, true ) )
		{
			if ( !pNode->m_dWords.GetLength() )
				pNode->m_dWords.Add ( tKeyword );
			pNode->m_dWords[0].m_sWord = sBuf;
			pNode->m_dWords[0].m_bMorphed = true;
			return;
		}
	}

	CSphVector<CSphString> dLemmas;
	DWORD uLangMask = tSettings.m_uAotFilterMask;
	for ( int i=AOT_BEGIN; i<AOT_LENGTH; ++i )
	{
		if ( uLangMask & (1UL<<i) )
		{
			if ( i==AOT_RU )
				sphAotLemmatizeRu ( dLemmas, (BYTE*)tKeyword.m_sWord.cstr(), bUtf8 );
			else if ( i==AOT_DE )
				sphAotLemmatizeDe ( dLemmas, (BYTE*)tKeyword.m_sWord.cstr(), bUtf8 );
			else
				sphAotLemmatize ( dLemmas, (BYTE*)tKeyword.m_sWord.cstr(), i );
		}
	}

	// post-morph wordforms
	if ( pWordforms && pWordforms->m_bHavePostMorphNF )
	{
		char sBuf [ MAX_KEYWORD_BYTES ];
		ARRAY_FOREACH ( i, dLemmas )
		{
			strncpy ( sBuf, dLemmas[i].cstr(), sizeof(sBuf) );
			if ( pWordforms->ToNormalForm ( (BYTE*)sBuf, false ) )
				dLemmas[i] = sBuf;
		}
	}

	if ( dLemmas.GetLength() && tSettings.m_bIndexExactWords )
	{
		pExact = CloneKeyword ( pNode );
		if ( !pExact->m_dWords.GetLength() )
			pExact->m_dWords.Add ( tKeyword );

		pExact->m_dWords[0].m_sWord.SetSprintf ( "=%s", tKeyword.m_sWord.cstr() );
		pExact->m_pParent = pNode;
	}

	if ( !pExact && dLemmas.GetLength()<=1 )
	{
		// zero or one lemmas, update node in-place
		if ( !pNode->m_dWords.GetLength() )
			pNode->m_dWords.Add ( tKeyword );
		if ( dLemmas.GetLength() )
		{
			pNode->m_dWords[0].m_sWord = dLemmas[0];
			pNode->m_dWords[0].m_bMorphed = true;
		}
	} else
	{
		// multiple lemmas, create an OR node
		pNode->SetOp ( SPH_QUERY_OR );
		ARRAY_FOREACH ( i, dLemmas )
		{
			pNode->m_dChildren.Add ( new XQNode_t ( pNode->m_dSpec ) );
			pNode->m_dChildren.Last()->m_pParent = pNode;
			XQKeyword_t & tLemma = pNode->m_dChildren.Last()->m_dWords.Add();
			tLemma.m_sWord = dLemmas[i];
			tLemma.m_iAtomPos = tKeyword.m_iAtomPos;
			tLemma.m_bFieldStart = tKeyword.m_bFieldStart;
			tLemma.m_bFieldEnd = tKeyword.m_bFieldEnd;
			tLemma.m_bMorphed = true;
		}
		pNode->m_dWords.Reset();
		if ( pExact )
			pNode->m_dChildren.Add ( pExact );
	}
}


/// AOT morph guesses transform
/// replaces tokens with their respective morph guesses subtrees
/// used in lemmatize_ru_all morphology processing mode that can generate multiple guesses
/// in other modes, there is always exactly one morph guess, and the dictionary handles it
/// Called from the daemon side (searchd)
void TransformAotFilter ( XQNode_t * pNode, bool bUtf8, const CSphWordforms * pWordforms, const CSphIndexSettings & tSettings )
{
	// case one, regular operator (and empty nodes)
	ARRAY_FOREACH ( i, pNode->m_dChildren )
		TransformAotFilter ( pNode->m_dChildren[i], bUtf8, pWordforms, tSettings );
	if ( pNode->m_dChildren.GetLength() || pNode->m_dWords.GetLength()==0 )
		return;

	// case two, operator on a bag of words
	// FIXME? check phrase vs expand_keywords vs lemmatize_ru_all?
	if ( pNode->m_dWords.GetLength()
		&& ( pNode->GetOp()==SPH_QUERY_PHRASE || pNode->GetOp()==SPH_QUERY_PROXIMITY || pNode->GetOp()==SPH_QUERY_QUORUM ) )
	{
		assert ( pNode->m_dWords.GetLength() );

		ARRAY_FOREACH ( i, pNode->m_dWords )
		{
			XQNode_t * pNew = new XQNode_t ( pNode->m_dSpec );
			pNew->m_pParent = pNode;
			pNew->m_iAtomPos = pNode->m_dWords[i].m_iAtomPos;
			pNode->m_dChildren.Add ( pNew );
			TransformAotFilterKeyword ( pNew, pNode->m_dWords[i], bUtf8, pWordforms, tSettings );
		}

		pNode->m_dWords.Reset();
		pNode->m_bVirtuallyPlain = true;
		return;
	}

	// case three, plain old single keyword
	assert ( pNode->m_dWords.GetLength()==1 );
	TransformAotFilterKeyword ( pNode, pNode->m_dWords[0], bUtf8, pWordforms, tSettings );
}


void sphTransformExtendedQuery ( XQNode_t ** ppNode, const CSphIndexSettings & tSettings, bool bHasBooleanOptimization, const ISphKeywordsStat * pKeywords )
{
	TransformQuorum ( ppNode );
	( *ppNode )->Check ( true );
	TransformNear ( ppNode );
	( *ppNode )->Check ( true );
	if ( tSettings.m_eBigramIndex!=SPH_BIGRAM_NONE )
		TransformBigrams ( *ppNode, tSettings );
	TagExcluded ( *ppNode, false );
	( *ppNode )->Check ( true );

	// boolean optimization
	if ( bHasBooleanOptimization )
		sphOptimizeBoolean ( ppNode, pKeywords );
}


struct CmpPSortersByRandom_fn
{
	inline bool IsLess ( const ISphMatchSorter * a, const ISphMatchSorter * b ) const
	{
		assert ( a );
		assert ( b );
		return a->m_bRandomize < b->m_bRandomize;
	}
};


/// one regular query vs many sorters
bool CSphIndex_VLN::MultiQuery ( const CSphQuery * pQuery, CSphQueryResult * pResult,
	int iSorters, ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const
{
	assert ( pQuery );
	CSphQueryProfile * pProfile = pResult->m_pProfile;

	MEMORY ( SPH_MEM_IDX_DISK_MULTY_QUERY );

	// to avoid the checking of a ppSorters's element for NULL on every next step, just filter out all nulls right here
	CSphVector<ISphMatchSorter*> dSorters;
	dSorters.Reserve ( iSorters );
	for ( int i=0; i<iSorters; i++ )
		if ( ppSorters[i] )
			dSorters.Add ( ppSorters[i] );

	iSorters = dSorters.GetLength();

	// if we have anything to work with
	if ( iSorters==0 )
		return false;

	// non-random at the start, random at the end
	dSorters.Sort ( CmpPSortersByRandom_fn() );

	// fast path for scans
	if ( pQuery->m_sQuery.IsEmpty() )
		return MultiScan ( pQuery, pResult, iSorters, &dSorters[0], tArgs );

	if ( pProfile )
		pProfile->Switch ( SPH_QSTATE_DICT_SETUP );

	CSphScopedPtr<CSphDict> tDictCloned ( NULL );
	CSphDict * pDictBase = m_pDict;
	if ( pDictBase->HasState() )
		tDictCloned = pDictBase = pDictBase->Clone();

	CSphScopedPtr<CSphDict> tDict ( NULL );
	CSphDict * pDict = SetupStarDict ( tDict, pDictBase );

	CSphScopedPtr<CSphDict> tDict2 ( NULL );
	pDict = SetupExactDict ( tDict2, pDict );

	CSphVector<BYTE> dFiltered;
	const BYTE * sModifiedQuery = (BYTE *)pQuery->m_sQuery.cstr();
	if ( m_pFieldFilter && m_pFieldFilter->Apply ( sModifiedQuery, 0, dFiltered ) )
		sModifiedQuery = dFiltered.Begin();

	// parse query
	if ( pProfile )
		pProfile->Switch ( SPH_QSTATE_PARSE );

	XQQuery_t tParsed;
	if ( !sphParseExtendedQuery ( tParsed, (const char*)sModifiedQuery, m_pQueryTokenizer, &m_tSchema, pDict, m_tSettings ) )
	{
		// FIXME? might wanna reset profile to unknown state
		pResult->m_sError = tParsed.m_sParseError;
		return false;
	}

	// transform query if needed (quorum transform, etc.)
	if ( pProfile )
		pProfile->Switch ( SPH_QSTATE_TRANSFORMS );
	sphTransformExtendedQuery ( &tParsed.m_pRoot, m_tSettings, pQuery->m_bSimplify, this );

	// adjust stars in keywords for dict=keywords, enable_star=0 case
	if ( pDict->GetSettings().m_bWordDict && !m_bEnableStar && ( m_tSettings.m_iMinPrefixLen>0 || m_tSettings.m_iMinInfixLen>0 ) )
		sphQueryAdjustStars ( tParsed.m_pRoot, m_tSettings );

	if ( m_bExpandKeywords )
	{
		tParsed.m_pRoot = sphQueryExpandKeywords ( tParsed.m_pRoot, m_tSettings, m_bEnableStar );
		tParsed.m_pRoot->Check ( true );
	}

	// this should be after keyword expansion
	if ( m_tSettings.m_uAotFilterMask )
		TransformAotFilter ( tParsed.m_pRoot, m_pQueryTokenizer->IsUtf8(), pDict->GetWordforms(), m_tSettings );

	// expanding prefix in word dictionary case
	XQNode_t * pPrefixed = ExpandPrefix ( tParsed.m_pRoot, pResult->m_sError, pResult );
	if ( !pPrefixed )
		return false;
	tParsed.m_pRoot = pPrefixed;

	if ( !sphCheckQueryHeight ( tParsed.m_pRoot, pResult->m_sError ) )
		return false;

	// flag common subtrees
	int iCommonSubtrees = 0;
	if ( m_iMaxCachedDocs && m_iMaxCachedHits )
		iCommonSubtrees = sphMarkCommonSubtrees ( 1, &tParsed );

	tParsed.m_bNeedSZlist = pQuery->m_bZSlist;

	CSphQueryNodeCache tNodeCache ( iCommonSubtrees, m_iMaxCachedDocs, m_iMaxCachedHits );
	bool bResult = ParsedMultiQuery ( pQuery, pResult, iSorters, &dSorters[0], tParsed, pDict, tArgs, &tNodeCache );

	return bResult;
}


/// many regular queries with one sorter attached to each query.
/// returns true if at least one query succeeded. The failed queries indicated with pResult->m_iMultiplier==-1
bool CSphIndex_VLN::MultiQueryEx ( int iQueries, const CSphQuery * pQueries,
	CSphQueryResult ** ppResults, ISphMatchSorter ** ppSorters, const CSphMultiQueryArgs & tArgs ) const
{
	// ensure we have multiple queries
	assert ( ppResults );
	if ( iQueries==1 )
		return MultiQuery ( pQueries, ppResults[0], 1, ppSorters, tArgs );

	MEMORY ( SPH_MEM_IDX_DISK_MULTY_QUERY_EX );

	assert ( pQueries );
	assert ( ppSorters );

	CSphScopedPtr<CSphDict> tDictCloned ( NULL );
	CSphDict * pDictBase = m_pDict;
	if ( pDictBase->HasState() )
		tDictCloned = pDictBase = pDictBase->Clone();

	CSphScopedPtr<CSphDict> tDict ( NULL );
	CSphDict * pDict = SetupStarDict ( tDict, pDictBase );

	CSphScopedPtr<CSphDict> tDict2 ( NULL );
	pDict = SetupExactDict ( tDict2, pDict );

	CSphFixedVector<XQQuery_t> dXQ ( iQueries );
	bool bResult = false;
	bool bResultScan = false;
	for ( int i=0; i<iQueries; i++ )
	{
		// nothing to do without a sorter
		if ( !ppSorters[i] )
		{
			ppResults[i]->m_iMultiplier = -1; ///< show that this particular query failed
			continue;
		}

		// fast path for scans
		if ( pQueries[i].m_sQuery.IsEmpty() )
		{
			if ( MultiScan ( pQueries + i, ppResults[i], 1, &ppSorters[i], tArgs ) )
				bResultScan = true;
			else
				ppResults[i]->m_iMultiplier = -1; ///< show that this particular query failed
			continue;
		}

		ppResults[i]->m_tIOStats.Start();

		// parse query
		if ( sphParseExtendedQuery ( dXQ[i], pQueries[i].m_sQuery.cstr(), m_pQueryTokenizer, &m_tSchema, pDict, m_tSettings ) )
		{
			// transform query if needed (quorum transform, keyword expansion, etc.)
			sphTransformExtendedQuery ( &dXQ[i].m_pRoot, m_tSettings, pQueries[i].m_bSimplify, this );

			// adjust stars in keywords for dict=keywords, enable_star=0 case
			if ( pDict->GetSettings().m_bWordDict && !m_bEnableStar && ( m_tSettings.m_iMinPrefixLen>0 || m_tSettings.m_iMinInfixLen>0 ) )
				sphQueryAdjustStars ( dXQ[i].m_pRoot, m_tSettings );

			if ( m_bExpandKeywords )
			{
				dXQ[i].m_pRoot = sphQueryExpandKeywords ( dXQ[i].m_pRoot, m_tSettings, m_bEnableStar );
				dXQ[i].m_pRoot->Check ( true );
			}

			// this should be after keyword expansion
			if ( m_tSettings.m_uAotFilterMask )
				TransformAotFilter ( dXQ[i].m_pRoot, m_pQueryTokenizer->IsUtf8(), pDict->GetWordforms(), m_tSettings );

			// expanding prefix in word dictionary case
			XQNode_t * pPrefixed = ExpandPrefix ( dXQ[i].m_pRoot, ppResults[i]->m_sError, ppResults[i] );
			if ( pPrefixed )
			{
				dXQ[i].m_pRoot = pPrefixed;

				if ( sphCheckQueryHeight ( dXQ[i].m_pRoot, ppResults[i]->m_sError ) )
				{
					bResult = true;
				} else
				{
					ppResults[i]->m_iMultiplier = -1;
					SafeDelete ( dXQ[i].m_pRoot );
				}
			} else
			{
				ppResults[i]->m_iMultiplier = -1;
				SafeDelete ( dXQ[i].m_pRoot );
			}
		} else
		{
			ppResults[i]->m_sError = dXQ[i].m_sParseError;
			ppResults[i]->m_iMultiplier = -1;
		}

		ppResults[i]->m_tIOStats.Stop();
	}

	// continue only if we have at least one non-failed
	if ( bResult )
	{
		int iCommonSubtrees = 0;
		if ( m_iMaxCachedDocs && m_iMaxCachedHits )
			iCommonSubtrees = sphMarkCommonSubtrees ( iQueries, &dXQ[0] );

		CSphQueryNodeCache tNodeCache ( iCommonSubtrees, m_iMaxCachedDocs, m_iMaxCachedHits );
		bResult = false;
		for ( int j=0; j<iQueries; j++ )
		{
			// fullscan case
			if ( pQueries[j].m_sQuery.IsEmpty() )
				continue;

			ppResults[j]->m_tIOStats.Start();

			if ( dXQ[j].m_pRoot && ppSorters[j]
					&& ParsedMultiQuery ( &pQueries[j], ppResults[j], 1, &ppSorters[j], dXQ[j], pDict, tArgs, &tNodeCache ) )
			{
				bResult = true;
				ppResults[j]->m_iMultiplier = iCommonSubtrees ? iQueries : 1;
			} else
			{
				ppResults[j]->m_iMultiplier = -1;
			}

			ppResults[j]->m_tIOStats.Stop();
		}
	}

	return bResult | bResultScan;
}

bool CSphIndex_VLN::ParsedMultiQuery ( const CSphQuery * pQuery, CSphQueryResult * pResult,
	int iSorters, ISphMatchSorter ** ppSorters, const XQQuery_t & tXQ, CSphDict * pDict,
	const CSphMultiQueryArgs & tArgs, CSphQueryNodeCache * pNodeCache ) const
{
	assert ( pQuery );
	assert ( pResult );
	assert ( ppSorters );
	assert ( !pQuery->m_sQuery.IsEmpty() && pQuery->m_eMode!=SPH_MATCH_FULLSCAN ); // scans must go through MultiScan()
	assert ( tArgs.m_iTag>=0 );

	// start counting
	int64_t tmQueryStart = sphMicroTimer();

	CSphQueryProfile * pProfile = pResult->m_pProfile;
	if ( pProfile )
		pProfile->Switch ( SPH_QSTATE_INIT );

	///////////////////
	// setup searching
	///////////////////

	// non-ready index, empty response!
	if ( !m_pPreread || !*m_pPreread )
	{
		pResult->m_sError = "index not preread";
		return false;
	}

	// select the sorter with max schema
	int iMaxSchemaSize = -1;
	int iMaxSchemaIndex = -1;
	for ( int i=0; i<iSorters; i++ )
		if ( ppSorters[i]->GetSchema().GetRowSize() > iMaxSchemaSize )
		{
			iMaxSchemaSize = ppSorters[i]->GetSchema().GetRowSize();
			iMaxSchemaIndex = i;
		}

	// setup calculations and result schema
	CSphQueryContext tCtx;
	tCtx.m_pProfile = pProfile;
	tCtx.m_pLocalDocs = tArgs.m_pLocalDocs;
	tCtx.m_iTotalDocs = tArgs.m_iTotalDocs;
	if ( !tCtx.SetupCalc ( pResult, ppSorters[iMaxSchemaIndex]->GetSchema(), m_tSchema, m_tMva.GetWritePtr() ) )
		return false;

	// set string pool for string on_sort expression fix up
	tCtx.SetStringPool ( m_tString.GetWritePtr() );

	tCtx.m_bPackedFactors = tArgs.m_bFactors;

	// open files
	CSphAutofile tDoclist, tHitlist, tWordlist, tDummy;
	if ( !m_bKeepFilesOpen )
	{
		if ( pProfile )
			pProfile->Switch ( SPH_QSTATE_OPEN );

		if ( tDoclist.Open ( GetIndexFileName("spd"), SPH_O_READ, pResult->m_sError ) < 0 )
			return false;

		if ( tHitlist.Open ( GetIndexFileName ( m_uVersion>=3 ? "spp" : "spd" ), SPH_O_READ, pResult->m_sError ) < 0 )
			return false;

		if ( tWordlist.Open ( GetIndexFileName ( "spi" ), SPH_O_READ, pResult->m_sError ) < 0 )
			return false;
	}

	if ( pProfile )
		pProfile->Switch ( SPH_QSTATE_INIT );

	// setup search terms
	DiskIndexQwordSetup_c tTermSetup ( m_bKeepFilesOpen ? m_tDoclistFile : tDoclist,
		m_bKeepFilesOpen ? m_tHitlistFile : tHitlist,
		m_bPreloadWordlist ? tDummy : ( m_bKeepFilesOpen ? m_tWordlist.m_tFile : tWordlist ),
		m_bPreloadWordlist ? 0 : m_tWordlist.m_iMaxChunk,
		m_pSkiplists.GetWritePtr(), pProfile );

	tTermSetup.m_pDict = pDict;
	tTermSetup.m_pIndex = this;
	tTermSetup.m_eDocinfo = m_tSettings.m_eDocinfo;
	tTermSetup.m_iMinDocid = m_iMinDocid;
	if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE )
	{
		tTermSetup.m_iInlineRowitems = m_tSchema.GetRowSize();
		tTermSetup.m_pMinRow = m_dMinRow.Begin();
	}
	tTermSetup.m_iDynamicRowitems = ppSorters[iMaxSchemaIndex]->GetSchema().GetDynamicSize();

	if ( pQuery->m_uMaxQueryMsec>0 )
		tTermSetup.m_iMaxTimer = sphMicroTimer() + pQuery->m_uMaxQueryMsec*1000; // max_query_time
	tTermSetup.m_pWarning = &pResult->m_sWarning;
	tTermSetup.m_bSetupReaders = true;
	tTermSetup.m_pCtx = &tCtx;
	tTermSetup.m_pNodeCache = pNodeCache;

	// setup prediction constrain
	CSphQueryStats tQueryStats;
	bool bCollectPredictionCounters = ( pQuery->m_iMaxPredictedMsec>0 );
	int64_t iNanoBudget = (int64_t)(pQuery->m_iMaxPredictedMsec) * 1000000; // from milliseconds to nanoseconds
	tQueryStats.m_pNanoBudget = &iNanoBudget;
	if ( bCollectPredictionCounters )
		tTermSetup.m_pStats = &tQueryStats;

	// bind weights
	tCtx.BindWeights ( pQuery, m_tSchema );

	SphWordStatChecker_t tStatDiff;
	tStatDiff.Set ( pResult->m_hWordStats );

	// setup query
	// must happen before index-level reject, in order to build proper keyword stats
	CSphScopedPtr<ISphRanker> pRanker ( sphCreateRanker ( tXQ, pQuery, pResult, tTermSetup, tCtx ) );
	if ( !pRanker.Ptr() )
		return false;

	tStatDiff.DumpDiffer ( pResult->m_hWordStats, m_sIndexName.cstr(), pResult->m_sWarning );

	if ( tArgs.m_bFactors && pQuery->m_eRanker!=SPH_RANK_EXPR )
		pResult->m_sWarning.SetSprintf ( "packedfactors() and bm25f() requires using an expression ranker" );

	tCtx.SetupExtraData ( pRanker.Ptr(), iSorters==1 ? ppSorters[0] : NULL );

	pRanker->ExtraData ( EXTRA_SET_MVAPOOL, (void**)m_tMva.GetWritePtr() );
	pRanker->ExtraData ( EXTRA_SET_STRINGPOOL, (void**)m_tString.GetWritePtr() );

	int iMatchPoolSize = 0;
	for ( int i=0; i<iSorters; i++ )
		iMatchPoolSize += ppSorters[i]->GetDataLength();

	pRanker->ExtraData ( EXTRA_SET_MAXMATCHES, (void**)&iMatchPoolSize );

	// empty index, empty response!
	if ( m_bIsEmpty )
		return true;
	assert ( m_tSettings.m_eDocinfo!=SPH_DOCINFO_EXTERN || !m_tAttr.IsEmpty() ); // check that docinfo is preloaded

	// setup filters
	if ( !tCtx.CreateFilters ( pQuery->m_sQuery.IsEmpty(), &pQuery->m_dFilters, ppSorters[iMaxSchemaIndex]->GetSchema(), m_tMva.GetWritePtr(), m_tString.GetWritePtr(), pResult->m_sError ) )
		return false;
	if ( !tCtx.CreateFilters ( pQuery->m_sQuery.IsEmpty(), tArgs.m_pExtraFilters, ppSorters[iMaxSchemaIndex]->GetSchema(), m_tMva.GetWritePtr(), m_tString.GetWritePtr(), pResult->m_sError ) )
		return false;

	// check if we can early reject the whole index
	if ( tCtx.m_pFilter && m_iDocinfoIndex )
	{
		DWORD uStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
		DWORD * pMinEntry = const_cast<DWORD*> ( &m_pDocinfoIndex [ m_iDocinfoIndex*uStride*2 ] );
		DWORD * pMaxEntry = pMinEntry + uStride;

		if ( !tCtx.m_pFilter->EvalBlock ( pMinEntry, pMaxEntry ) )
			return true;
	}

	// setup lookup
	tCtx.m_bLookupFilter = ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN ) && pQuery->m_dFilters.GetLength();
	if ( tCtx.m_dCalcFilter.GetLength() || pQuery->m_eRanker==SPH_RANK_EXPR || pQuery->m_eRanker==SPH_RANK_EXPORT )
		tCtx.m_bLookupFilter = true; // suboptimal in case of attr-independent expressions, but we don't care

	tCtx.m_bLookupSort = false;
	if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN && !tCtx.m_bLookupFilter )
		for ( int iSorter=0; iSorter<iSorters && !tCtx.m_bLookupSort; iSorter++ )
			if ( ppSorters[iSorter]->UsesAttrs() )
				tCtx.m_bLookupSort = true;
	if ( tCtx.m_dCalcSort.GetLength() )
		tCtx.m_bLookupSort = true; // suboptimal in case of attr-independent expressions, but we don't care

	// setup sorters vs. MVA
	for ( int i=0; i<iSorters; i++ )
	{
		(ppSorters[i])->SetMVAPool ( m_tMva.GetWritePtr() );
		(ppSorters[i])->SetStringPool ( m_tString.GetWritePtr() );
	}

	// setup overrides
	if ( !tCtx.SetupOverrides ( pQuery, pResult, m_tSchema, ppSorters[iMaxSchemaIndex]->GetSchema() ) )
		return false;

	//////////////////////////////////////
	// find and weight matching documents
	//////////////////////////////////////

	bool bFinalLookup = !tCtx.m_bLookupFilter && !tCtx.m_bLookupSort;
	bool bFinalPass = bFinalLookup || tCtx.m_dCalcFinal.GetLength();
	int iMyTag = bFinalPass ? -1 : tArgs.m_iTag;

	switch ( pQuery->m_eMode )
	{
		case SPH_MATCH_ALL:
		case SPH_MATCH_PHRASE:
		case SPH_MATCH_ANY:
		case SPH_MATCH_EXTENDED:
		case SPH_MATCH_EXTENDED2:
		case SPH_MATCH_BOOLEAN:
			MatchExtended ( &tCtx, pQuery, iSorters, ppSorters, pRanker.Ptr(), iMyTag, tArgs.m_iIndexWeight );
			break;

		default:
			sphDie ( "INTERNAL ERROR: unknown matching mode (mode=%d)", pQuery->m_eMode );
	}

	////////////////////
	// cook result sets
	////////////////////

	if ( pProfile )
		pProfile->Switch ( SPH_QSTATE_FINALIZE );

	// adjust result sets
	if ( bFinalPass )
	{
		// GotUDF means promise to UDFs that final-stage calls will be evaluated
		// a) over the final, pre-limit result set
		// b) in the final result set order
		bool bGotUDF = false;
		ARRAY_FOREACH_COND ( i, tCtx.m_dCalcFinal, !bGotUDF )
			tCtx.m_dCalcFinal[i].m_pExpr->Command ( SPH_EXPR_GET_UDF, &bGotUDF );

		SphFinalMatchCalc_t tProcessor ( tArgs.m_iTag, bFinalLookup ? this : NULL, tCtx );
		for ( int iSorter=0; iSorter<iSorters; iSorter++ )
		{
			ISphMatchSorter * pTop = ppSorters[iSorter];
			pTop->Finalize ( tProcessor, bGotUDF );
		}
	}

	// mva and string pools ptrs
	pResult->m_pMva = m_tMva.GetWritePtr();
	pResult->m_pStrings = m_tString.GetWritePtr();

	// query timer
	int64_t tmWall = sphMicroTimer() - tmQueryStart;
	pResult->m_iQueryTime += (int)( tmWall/1000 );

#if 0
	printf ( "qtm %d, %d, %d, %d, %d\n", int(tmWall), tQueryStats.m_iFetchedDocs,
		tQueryStats.m_iFetchedHits, tQueryStats.m_iSkips, ppSorters[0]->GetTotalCount() );
#endif

	if ( pProfile )
		pProfile->Switch ( SPH_QSTATE_UNKNOWN );

	if ( bCollectPredictionCounters )
	{
		pResult->m_tStats.m_iFetchedDocs += tQueryStats.m_iFetchedDocs;
		pResult->m_tStats.m_iFetchedHits += tQueryStats.m_iFetchedHits;
		pResult->m_tStats.m_iSkips += tQueryStats.m_iSkips;
		pResult->m_bHasPrediction = true;
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// INDEX STATUS
//////////////////////////////////////////////////////////////////////////

CSphIndexStatus CSphIndex_VLN::GetStatus () const
{
	CSphIndexStatus tRes;
	tRes.m_iRamUse = sizeof(CSphIndex_VLN)
		+ m_dMinRow.GetSizeBytes()
		+ m_dFieldLens.GetSizeBytes()

		+ m_pDocinfoHash.GetLengthBytes()
		+ m_tAttr.GetLengthBytes()
		+ m_tMva.GetLengthBytes()
		+ m_tString.GetLengthBytes()
		+ m_tWordlist.m_pBuf.GetLengthBytes()
		+ m_pKillList.GetLengthBytes()
		+ m_pSkiplists.GetLengthBytes()
		+ m_dShared.GetLengthBytes();

	char sFile [ SPH_MAX_FILENAME_LEN ];
	tRes.m_iDiskUse = 0;
	for ( int i=0; i<sphGetExtCount ( m_uVersion ); i++ )
	{
		snprintf ( sFile, sizeof(sFile), "%s%s", m_sFilename.cstr(), sphGetExts ( SPH_EXT_TYPE_CUR, m_uVersion )[i] );
		struct_stat st;
		if ( stat ( sFile, &st )==0 )
			tRes.m_iDiskUse += st.st_size;
	}

	return tRes;
}

//////////////////////////////////////////////////////////////////////////
// INDEX CHECKING
//////////////////////////////////////////////////////////////////////////

// no strnlen on some OSes (Mac OS)
#if !HAVE_STRNLEN
size_t strnlen ( const char * s, size_t iMaxLen )
{
	if ( !s )
		return 0;

	size_t iRes = 0;
	while ( *s++ && iRes<iMaxLen )
		++iRes;
	return iRes;
}
#endif


#define LOC_FAIL(_args) \
	if ( ++iFails<=FAILS_THRESH ) \
	{ \
		fprintf ( fp, "FAILED, " ); \
		fprintf _args; \
		fprintf ( fp, "\n" ); \
		iFailsPrinted++; \
		\
		if ( iFails==FAILS_THRESH ) \
			fprintf ( fp, "(threshold reached; suppressing further output)\n" ); \
	}

int CSphIndex_VLN::DebugCheck ( FILE * fp )
{
	int64_t tmCheck = sphMicroTimer();
	int64_t iFails = 0;
	int iFailsPrinted = 0;
	const int FAILS_THRESH = 100;

	// check if index is ready
	if ( m_dShared.GetNumEntries()!=SPH_SHARED_VARS_COUNT || !m_pPreread || !*m_pPreread )
		LOC_FAIL(( fp, "index not preread" ));

	bool bProgress = isatty ( fileno ( fp ) )!=0;

	//////////////
	// open files
	//////////////

	CSphString sError;
	CSphAutoreader rdDict, rdDocs, rdHits;

	if ( !rdDict.Open ( GetIndexFileName("spi"), sError ) )
		LOC_FAIL(( fp, "unable to open dictionary: %s", sError.cstr() ));

	if ( !rdDocs.Open ( GetIndexFileName("spd"), sError ) )
		LOC_FAIL(( fp, "unable to open doclist: %s", sError.cstr() ));

	if ( !rdHits.Open ( GetIndexFileName("spp"), sError ) )
		LOC_FAIL(( fp, "unable to open hitlist: %s", sError.cstr() ));

	CSphVector<SphWordID_t> dHitlessWords;
	if ( !LoadHitlessWords ( dHitlessWords ) )
		LOC_FAIL(( fp, "unable to load hitless words: %s", m_sLastError.cstr() ));

	CSphSavedFile tStat;
	const CSphTokenizerSettings & tTokenizerSettings = m_pTokenizer->GetSettings ();
	if ( !tTokenizerSettings.m_sSynonymsFile.IsEmpty() && !GetFileStats ( tTokenizerSettings.m_sSynonymsFile.cstr(), tStat, &sError ) )
		LOC_FAIL(( fp, "unable to open exceptions '%s': %s", tTokenizerSettings.m_sSynonymsFile.cstr(), sError.cstr() ));

	const CSphDictSettings & tDictSettings = m_pDict->GetSettings ();
	if ( !tDictSettings.m_sStopwords.IsEmpty() && !GetFileStats ( tDictSettings.m_sStopwords.cstr(), tStat, &sError ) )
		LOC_FAIL(( fp, "unable to open stopwords '%s': %s", tDictSettings.m_sStopwords.cstr(), sError.cstr() ));
	if ( !tDictSettings.m_dWordforms.GetLength() )
	{
		ARRAY_FOREACH ( i, tDictSettings.m_dWordforms )
		{
			if ( !GetFileStats ( tDictSettings.m_dWordforms[i].cstr(), tStat, &sError ) )
				LOC_FAIL(( fp, "unable to open wordforms '%s': %s", tDictSettings.m_dWordforms[i].cstr(), sError.cstr() ));
		}
	}

	////////////////////
	// check dictionary
	////////////////////

	fprintf ( fp, "checking dictionary...\n" );

	SphWordID_t uWordid = 0;
	int64_t iDoclistOffset = 0;
	int iWordsTotal = 0;

	char sWord[MAX_KEYWORD_BYTES], sLastWord[MAX_KEYWORD_BYTES];
	memset ( sWord, 0, sizeof(sWord) );
	memset ( sLastWord, 0, sizeof(sLastWord) );

	const int iWordPerCP = m_uVersion>=21 ? SPH_WORDLIST_CHECKPOINT : 1024;
	const bool bWordDict = m_pDict->GetSettings().m_bWordDict;

	CSphVector<CSphWordlistCheckpoint> dCheckpoints;

	if ( bWordDict && m_uVersion<21 )
		LOC_FAIL(( fp, "dictionary needed index version not less then 21 (readed=%d)"
			, m_uVersion ));

	int iLastSkipsOffset = 0;
	rdDict.SeekTo ( 1, READ_NO_SIZE_HINT );
	SphOffset_t iWordsEnd = m_tWordlist.m_iWordsEnd;
	bool bCheckInfixes = bWordDict && m_tWordlist.m_iInfixCodepointBytes && m_tWordlist.m_dInfixBlocks.GetLength();
	bool bUtf8 = ( m_pTokenizer && m_pTokenizer->IsUtf8() );
	CSphVector<int> dInfix2CP;

	while ( rdDict.GetPos()!=iWordsEnd && !m_bIsEmpty )
	{
		// sanity checks
		if ( rdDict.GetPos()>=iWordsEnd )
		{
			LOC_FAIL(( fp, "reading past checkpoints" ));
			break;
		}

		// store current entry pos (for checkpointing later), read next delta
		const int64_t iDictPos = rdDict.GetPos();
		const SphWordID_t iDeltaWord = bWordDict ? rdDict.GetByte() : rdDict.UnzipWordid();

		// checkpoint encountered, handle it
		if ( !iDeltaWord )
		{
			rdDict.UnzipOffset();

			if ( ( iWordsTotal%iWordPerCP )!=0 && rdDict.GetPos()!=iWordsEnd )
				LOC_FAIL(( fp, "unexpected checkpoint (pos="INT64_FMT", word=%d, words=%d, expected=%d)",
					iDictPos, iWordsTotal, ( iWordsTotal%iWordPerCP ), iWordPerCP ));

			uWordid = 0;
			iDoclistOffset = 0;
			continue;
		}

		SphWordID_t uNewWordid = 0;
		SphOffset_t iNewDoclistOffset = 0;
		int iDocs = 0;
		int iHits = 0;

		if ( bWordDict )
		{
			// unpack next word
			// must be in sync with DictEnd()!
			BYTE uPack = (BYTE)iDeltaWord;
			int iMatch, iDelta;
			if ( uPack & 0x80 )
			{
				iDelta = ( ( uPack>>4 ) & 7 ) + 1;
				iMatch = uPack & 15;
			} else
			{
				iDelta = uPack & 127;
				iMatch = rdDict.GetByte();
			}
			const int iLastWordLen = strlen(sLastWord);
			if ( iMatch+iDelta>=(int)sizeof(sLastWord)-1 || iMatch>iLastWordLen )
			{
				LOC_FAIL(( fp, "wrong word-delta (pos="INT64_FMT", word=%s, len=%d, begin=%d, delta=%d)",
					iDictPos, sLastWord, iLastWordLen, iMatch, iDelta ));
				rdDict.SkipBytes ( iDelta );
			} else
			{
				rdDict.GetBytes ( sWord + iMatch, iDelta );
				sWord [ iMatch+iDelta ] = '\0';
			}

			iNewDoclistOffset = rdDict.UnzipOffset();
			iDocs = rdDict.UnzipInt();
			iHits = rdDict.UnzipInt();
			int iHint = ( iDocs>=DOCLIST_HINT_THRESH ) ? rdDict.GetByte() : 0;
			iHint = DoclistHintUnpack ( iDocs, (BYTE)iHint );

			const int iNewWordLen = strlen(sWord);

			if ( iNewWordLen==0 )
				LOC_FAIL(( fp, "empty word in dictionary (pos="INT64_FMT")",
					iDictPos ));

			if ( iLastWordLen && iNewWordLen )
				if ( sphDictCmpStrictly ( sWord, iNewWordLen, sLastWord, iLastWordLen )<=0 )
					LOC_FAIL(( fp, "word order decreased (pos="INT64_FMT", word=%s, prev=%s)",
						iDictPos, sLastWord, sWord ));

			if ( iHint<0 )
				LOC_FAIL(( fp, "invalid word hint (pos="INT64_FMT", word=%s, hint=%d)",
					iDictPos, sWord, iHint ));

			if ( iDocs<=0 || iHits<=0 || iHits<iDocs )
				LOC_FAIL(( fp, "invalid docs/hits (pos="INT64_FMT", word=%s, docs="INT64_FMT", hits="INT64_FMT")",
					(int64_t)iDictPos, sWord, (int64_t)iDocs, (int64_t)iHits ));

			memcpy ( sLastWord, sWord, sizeof(sLastWord) );
		} else
		{
			// finish reading the entire entry
			uNewWordid = uWordid + iDeltaWord;
			iNewDoclistOffset = iDoclistOffset + rdDict.UnzipOffset();
			iDocs = rdDict.UnzipInt();
			iHits = rdDict.UnzipInt();
			bool bHitless = ( dHitlessWords.BinarySearch ( uNewWordid )!=NULL );
			if ( bHitless )
				iDocs &= 0x7fffffff;

			if ( uNewWordid<=uWordid )
				LOC_FAIL(( fp, "wordid decreased (pos="INT64_FMT", wordid="UINT64_FMT", previd="UINT64_FMT")",
					(int64_t)iDictPos, (uint64_t)uNewWordid, (uint64_t)uWordid ));

			if ( iNewDoclistOffset<=iDoclistOffset )
				LOC_FAIL(( fp, "doclist offset decreased (pos="INT64_FMT", wordid="UINT64_FMT")",
					(int64_t)iDictPos, (uint64_t)uNewWordid ));

			if ( iDocs<=0 || iHits<=0 || iHits<iDocs )
				LOC_FAIL(( fp, "invalid docs/hits (pos="INT64_FMT", wordid="UINT64_FMT", docs="INT64_FMT", hits="INT64_FMT", hitless=%s)",
					(int64_t)iDictPos, (uint64_t)uNewWordid, (int64_t)iDocs, (int64_t)iHits, ( bHitless?"true":"false" ) ));
		}

		// skiplist
		if ( m_bHaveSkips && iDocs>SPH_SKIPLIST_BLOCK )
		{
			int iSkipsOffset = rdDict.UnzipInt();
			if ( !bWordDict && iSkipsOffset<iLastSkipsOffset )
				LOC_FAIL(( fp, "descending skiplist pos (last=%d, cur=%d, wordid=%llu)",
					iLastSkipsOffset, iSkipsOffset, UINT64 ( uNewWordid ) ));
			iLastSkipsOffset = iSkipsOffset;
		}

		// update stats, add checkpoint
		if ( ( iWordsTotal%iWordPerCP )==0 )
		{
			CSphWordlistCheckpoint & tCP = dCheckpoints.Add();
			tCP.m_iWordlistOffset = iDictPos;
			if ( bWordDict )
			{
				const int iLen = strlen ( sWord );
				char * sWordChecked = new char [iLen+1];
				strncpy ( sWordChecked, sWord, iLen+1 );
				tCP.m_sWord = sWordChecked;
			} else
				tCP.m_iWordID = uNewWordid;
		}

		// check infixes
		if ( bCheckInfixes )
		{
			int iWordBytes = strnlen ( sWord, sizeof(sWord) );
			int iWordCodepoints = bUtf8 ? sphUTF8Len ( sWord ) : iWordBytes;

			if ( iWordCodepoints>=m_tSettings.m_iMinInfixLen )
			{
				dInfix2CP.Resize ( 0 );

				int iInfixBytes = sphGetInfixLength ( sWord, iWordBytes, m_tWordlist.m_iInfixCodepointBytes );
				sphLookupInfixCheckpoints ( sWord, iInfixBytes, m_tWordlist.m_pBuf.GetWritePtr(), m_tWordlist.m_dInfixBlocks,
					m_tWordlist.m_iInfixCodepointBytes, dInfix2CP );

				if ( !dInfix2CP.BinarySearch ( dCheckpoints.GetLength() ) )
				{
					LOC_FAIL(( fp, "infix not found for word '%s' (%d), checkpoint %d, readpos="INT64_FMT,
						sWord, iWordsTotal, dCheckpoints.GetLength(), (int64_t)iDictPos ));
				}
			}
		}

		uWordid = uNewWordid;
		iDoclistOffset = iNewDoclistOffset;
		iWordsTotal++;
	}

	// check the checkpoints
	if ( dCheckpoints.GetLength()!=m_tWordlist.m_dCheckpoints.GetLength() )
		LOC_FAIL(( fp, "checkpoint count mismatch (read=%d, calc=%d)",
			m_tWordlist.m_dCheckpoints.GetLength(), dCheckpoints.GetLength() ));

	for ( int i=0; i < Min ( dCheckpoints.GetLength(), m_tWordlist.m_dCheckpoints.GetLength() ); i++ )
	{
		const CSphWordlistCheckpoint & tRefCP = dCheckpoints[i];
		const CSphWordlistCheckpoint & tCP = m_tWordlist.m_dCheckpoints[i];
		const int iLen = bWordDict ? strlen ( tCP.m_sWord ) : 0;
		if ( bWordDict && ( tRefCP.m_sWord[0]=='\0' || tCP.m_sWord[0]=='\0' ) )
		{
			LOC_FAIL(( fp, "empty checkpoint %d (read_word=%s, read_len=%u, readpos="INT64_FMT", calc_word=%s, calc_len=%u, calcpos="INT64_FMT")",
				i, tCP.m_sWord, (DWORD)strlen ( tCP.m_sWord ), (int64_t)tCP.m_iWordlistOffset,
					tRefCP.m_sWord, (DWORD)strlen ( tRefCP.m_sWord ), (int64_t)tRefCP.m_iWordlistOffset ));

		} else if ( sphCheckpointCmpStrictly ( tCP.m_sWord, iLen, tCP.m_iWordID, bWordDict, tRefCP )
			|| tRefCP.m_iWordlistOffset!=tCP.m_iWordlistOffset )
		{
			if ( bWordDict )
			{
				LOC_FAIL(( fp, "checkpoint %d differs (read_word=%s, readpos="INT64_FMT", calc_word=%s, calcpos="INT64_FMT")",
					i,
					tCP.m_sWord,
					(int64_t)tCP.m_iWordlistOffset,
					tRefCP.m_sWord,
					(int64_t)tRefCP.m_iWordlistOffset ));
			} else
			{
				LOC_FAIL(( fp, "checkpoint %d differs (readid="UINT64_FMT", readpos="INT64_FMT", calcid="UINT64_FMT", calcpos="INT64_FMT")",
					i,
					(uint64_t)tCP.m_iWordID,
					(int64_t)tCP.m_iWordlistOffset,
					(uint64_t)tRefCP.m_iWordID,
					(int64_t)tRefCP.m_iWordlistOffset ));
			}
		}
	}
	if ( bWordDict )
		ARRAY_FOREACH ( i, dCheckpoints )
			SafeDeleteArray ( dCheckpoints[i].m_sWord );

	dCheckpoints.Reset ();

	///////////////////////
	// check docs and hits
	///////////////////////

	fprintf ( fp, "checking data...\n" );

	int64_t iDocsSize = rdDocs.GetFilesize();

	rdDict.SeekTo ( 1, READ_NO_SIZE_HINT );
	rdDocs.SeekTo ( 1, READ_NO_SIZE_HINT );
	rdHits.SeekTo ( 1, READ_NO_SIZE_HINT );

	uWordid = 0;
	iDoclistOffset = 0;
	int iDictDocs, iDictHits;
	bool bHitless = false;

	int iWordsChecked = 0;
	while ( rdDict.GetPos()<iWordsEnd )
	{
		const SphWordID_t iDeltaWord = bWordDict ? rdDict.GetByte() : rdDict.UnzipWordid();
		if ( !iDeltaWord )
		{
			rdDict.UnzipOffset();

			uWordid = 0;
			iDoclistOffset = 0;
			continue;
		}

		if ( bWordDict )
		{
			// unpack next word
			// must be in sync with DictEnd()!
			BYTE uPack = (BYTE)iDeltaWord;

			int iMatch, iDelta;
			if ( uPack & 0x80 )
			{
				iDelta = ( ( uPack>>4 ) & 7 ) + 1;
				iMatch = uPack & 15;
			} else
			{
				iDelta = uPack & 127;
				iMatch = rdDict.GetByte();
			}
			const int iLastWordLen = strlen(sWord);
			if ( iMatch+iDelta>=(int)sizeof(sWord)-1 || iMatch>iLastWordLen )
				rdDict.SkipBytes ( iDelta );
			else
			{
				rdDict.GetBytes ( sWord + iMatch, iDelta );
				sWord [ iMatch+iDelta ] = '\0';
			}

			iDoclistOffset = rdDict.UnzipOffset();
			iDictDocs = rdDict.UnzipInt();
			iDictHits = rdDict.UnzipInt();
			if ( iDictDocs>=DOCLIST_HINT_THRESH )
				rdDict.GetByte();
		} else
		{
			// finish reading the entire entry
			uWordid = uWordid + iDeltaWord;
			bHitless = ( dHitlessWords.BinarySearch ( uWordid )!=NULL );
			iDoclistOffset = iDoclistOffset + rdDict.UnzipOffset();
			iDictDocs = rdDict.UnzipInt();
			if ( bHitless )
				iDictDocs &= 0x7fffffff;
			iDictHits = rdDict.UnzipInt();
		}

		// FIXME? verify skiplist content too
		int iSkipsOffset = 0;
		if ( m_bHaveSkips && iDictDocs>SPH_SKIPLIST_BLOCK )
			iSkipsOffset = rdDict.UnzipInt();

		// check whether the offset is as expected
		if ( iDoclistOffset!=rdDocs.GetPos() )
		{
			if ( !bWordDict )
				LOC_FAIL(( fp, "unexpected doclist offset (wordid="UINT64_FMT"(%s)(%d), dictpos="INT64_FMT", doclistpos="INT64_FMT")",
					(uint64_t)uWordid, sWord, iWordsChecked, iDoclistOffset, (int64_t)rdDocs.GetPos() ));

			if ( iDoclistOffset>=iDocsSize || iDoclistOffset<0 )
			{
				LOC_FAIL(( fp, "unexpected doclist offset, off the file (wordid="UINT64_FMT"(%s)(%d), dictpos="INT64_FMT", doclistsize="INT64_FMT")",
					(uint64_t)uWordid, sWord, iWordsChecked, iDoclistOffset, iDocsSize ));
				iWordsChecked++;
				continue;
			} else
				rdDocs.SeekTo ( iDoclistOffset, READ_NO_SIZE_HINT );
		}

		// create and manually setup doclist reader
		DiskIndexQwordTraits_c * pQword = NULL;
		DWORD uInlineHits = ( m_tSettings.m_eHitFormat==SPH_HIT_FORMAT_INLINE );
		DWORD uInlineDocinfo = ( m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE );
		switch ( ( uInlineHits<<1 ) | uInlineDocinfo )
		{
		case 0: { typedef DiskIndexQword_c < false, false, false, true > T; pQword = new T ( false, false ); break; }
		case 1: { typedef DiskIndexQword_c < false, true, false, true > T; pQword = new T ( false, false ); break; }
		case 2: { typedef DiskIndexQword_c < true, false, false, true > T; pQword = new T ( false, false ); break; }
		case 3: { typedef DiskIndexQword_c < true, true, false, true > T; pQword = new T ( false, false ); break; }
		}
		if ( !pQword )
			sphDie ( "INTERNAL ERROR: impossible qword settings" );

		pQword->m_tDoc.Reset ( m_tSchema.GetDynamicSize() );
		pQword->m_iMinID = m_iMinDocid;
		pQword->m_tDoc.m_iDocID = m_iMinDocid;
		if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_INLINE )
		{
			pQword->m_iInlineAttrs = m_tSchema.GetDynamicSize();
			pQword->m_pInlineFixup = m_dMinRow.Begin();
		} else
		{
			pQword->m_iInlineAttrs = 0;
			pQword->m_pInlineFixup = NULL;
		}
		pQword->m_iDocs = 0;
		pQword->m_iHits = 0;
		pQword->m_rdDoclist.SetFile ( rdDocs.GetFD(), rdDocs.GetFilename().cstr() );
		pQword->m_rdDoclist.SeekTo ( rdDocs.GetPos(), READ_NO_SIZE_HINT );
		pQword->m_rdHitlist.SetFile ( rdHits.GetFD(), rdHits.GetFilename().cstr() );
		pQword->m_rdHitlist.SeekTo ( rdHits.GetPos(), READ_NO_SIZE_HINT );

		CSphRowitem * pInlineStorage = NULL;
		if ( pQword->m_iInlineAttrs )
			pInlineStorage = new CSphRowitem [ pQword->m_iInlineAttrs ];

		// loop the doclist
		SphDocID_t uLastDocid = 0;
		int iDoclistDocs = 0;
		int iDoclistHits = 0;
		int iHitlistHits = 0;

		// FIXME!!! dict=keywords + hitless_words=some
		bHitless = ( m_tSettings.m_eHitless==SPH_HITLESS_ALL ||
			( m_tSettings.m_eHitless==SPH_HITLESS_SOME && dHitlessWords.BinarySearch ( uWordid ) ) );
		pQword->m_bHasHitlist = !bHitless;

		CSphVector<SkiplistEntry_t> dDoclistSkips;
		for ( ;; )
		{
			// skiplist state is saved just *before* decoding those boundary entries
			if ( m_bHaveSkips && ( iDoclistDocs & ( SPH_SKIPLIST_BLOCK-1 ) )==0 )
			{
				SkiplistEntry_t & tBlock = dDoclistSkips.Add();
				tBlock.m_iBaseDocid = pQword->m_tDoc.m_iDocID;
				tBlock.m_iOffset = pQword->m_rdDoclist.GetPos();
				tBlock.m_iBaseHitlistPos = pQword->m_uHitPosition;
			}

			// FIXME? this can fail on a broken entry (eg fieldid over 256)
			const CSphMatch & tDoc = pQword->GetNextDoc ( pInlineStorage );
			if ( !tDoc.m_iDocID )
				break;

			// checks!
			if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN )
			{
				const CSphRowitem * pFound = FindDocinfo ( tDoc.m_iDocID );
				if ( !pFound )
					LOC_FAIL(( fp, "row not found (wordid="UINT64_FMT"(%s), docid="DOCID_FMT")",
						uint64_t(uWordid), sWord, tDoc.m_iDocID ));

				if ( pFound )
					if ( tDoc.m_iDocID!=DOCINFO2ID(pFound) )
						LOC_FAIL(( fp, "row found but id mismatches (wordid="UINT64_FMT"(%s), docid="DOCID_FMT", found="DOCID_FMT")",
							uint64_t(uWordid), sWord, tDoc.m_iDocID, DOCINFO2ID(pFound) ));
			}

			if ( tDoc.m_iDocID<=uLastDocid )
				LOC_FAIL(( fp, "docid decreased (wordid="UINT64_FMT"(%s), docid="DOCID_FMT", lastid="DOCID_FMT")",
					uint64_t(uWordid), sWord, tDoc.m_iDocID, uLastDocid ));

			uLastDocid = tDoc.m_iDocID;
			iDoclistDocs++;
			iDoclistHits += pQword->m_uMatchHits;

			// check position in case of regular (not-inline) hit
			if (!( pQword->m_iHitlistPos>>63 ))
			{
				if ( !bWordDict && pQword->m_iHitlistPos!=pQword->m_rdHitlist.GetPos() )
					LOC_FAIL(( fp, "unexpected hitlist offset (wordid="UINT64_FMT"(%s), docid="DOCID_FMT", expected="INT64_FMT", actual="INT64_FMT")",
						(uint64_t)uWordid, sWord, pQword->m_tDoc.m_iDocID,
						(int64_t)pQword->m_iHitlistPos, (int64_t)pQword->m_rdHitlist.GetPos() ));
			}

			// aim
			pQword->SeekHitlist ( pQword->m_iHitlistPos );

			// loop the hitlist
			int iDocHits = 0;
			FieldMask_t dFieldMask;
			dFieldMask.UnsetAll();
			Hitpos_t uLastHit = EMPTY_HIT;

			while ( !bHitless )
			{
				Hitpos_t uHit = pQword->GetNextHit();
				if ( uHit==EMPTY_HIT )
					break;

				if ( !( uLastHit<uHit ) )
					LOC_FAIL(( fp, "hit entries sorting order decreased (wordid="UINT64_FMT"(%s), docid="DOCID_FMT", hit=%u, last=%u)",
							(uint64_t)uWordid, sWord, pQword->m_tDoc.m_iDocID, uHit, uLastHit ));

				if ( HITMAN::GetField ( uLastHit )==HITMAN::GetField ( uHit ) )
				{
					if ( !( HITMAN::GetPos ( uLastHit )<HITMAN::GetPos ( uHit ) ) )
						LOC_FAIL(( fp, "hit decreased (wordid="UINT64_FMT"(%s), docid="DOCID_FMT", hit=%u, last=%u)",
								(uint64_t)uWordid, sWord, pQword->m_tDoc.m_iDocID, HITMAN::GetPos ( uHit ), HITMAN::GetPos ( uLastHit ) ));
					if ( HITMAN::IsEnd ( uLastHit ) )
						LOC_FAIL(( fp, "multiple tail hits (wordid="UINT64_FMT"(%s), docid="DOCID_FMT", hit=0x%x, last=0x%x)",
								(uint64_t)uWordid, sWord, pQword->m_tDoc.m_iDocID, uHit, uLastHit ));
				} else
				{
					if ( !( HITMAN::GetField ( uLastHit )<HITMAN::GetField ( uHit ) ) )
						LOC_FAIL(( fp, "hit field decreased (wordid="UINT64_FMT"(%s), docid="DOCID_FMT", hit field=%u, last field=%u)",
								(uint64_t)uWordid, sWord, pQword->m_tDoc.m_iDocID, HITMAN::GetField ( uHit ), HITMAN::GetField ( uLastHit ) ));
				}

				uLastHit = uHit;

				int iField = HITMAN::GetField ( uHit );
				if ( iField<0 || iField>=SPH_MAX_FIELDS )
				{
					LOC_FAIL(( fp, "hit field out of bounds (wordid="UINT64_FMT"(%s), docid="DOCID_FMT", field=%d)",
						(uint64_t)uWordid, sWord, pQword->m_tDoc.m_iDocID, iField ));

				} else if ( iField>=m_tSchema.m_dFields.GetLength() )
				{
					LOC_FAIL(( fp, "hit field out of schema (wordid="UINT64_FMT"(%s), docid="DOCID_FMT", field=%d)",
						(uint64_t)uWordid, sWord, pQword->m_tDoc.m_iDocID, iField ));
				} else
				{
					dFieldMask.Set(iField);
				}

				iDocHits++; // to check doclist entry
				iHitlistHits++; // to check dictionary entry
			}

			// check hit count
			if ( iDocHits!=(int)pQword->m_uMatchHits && !bHitless )
				LOC_FAIL(( fp, "doc hit count mismatch (wordid="UINT64_FMT"(%s), docid="DOCID_FMT", doclist=%d, hitlist=%d)",
					(uint64_t)uWordid, sWord, pQword->m_tDoc.m_iDocID, pQword->m_uMatchHits, iDocHits ));

			// check the mask
			if ( memcmp ( dFieldMask.m_dMask, pQword->m_dQwordFields.m_dMask, sizeof(dFieldMask.m_dMask) ) && !bHitless )
				LOC_FAIL(( fp, "field mask mismatch (wordid="UINT64_FMT"(%s), docid="DOCID_FMT")",
					(uint64_t)uWordid, sWord, pQword->m_tDoc.m_iDocID ));

			// update my hitlist reader
			rdHits.SeekTo ( pQword->m_rdHitlist.GetPos(), READ_NO_SIZE_HINT );
		}

		// do checks
		if ( iDictDocs!=iDoclistDocs )
			LOC_FAIL(( fp, "doc count mismatch (wordid="UINT64_FMT"(%s), dict=%d, doclist=%d, hitless=%s)",
				uint64_t(uWordid), sWord, iDictDocs, iDoclistDocs, ( bHitless?"true":"false" ) ));

		if ( ( iDictHits!=iDoclistHits || iDictHits!=iHitlistHits ) && !bHitless )
			LOC_FAIL(( fp, "hit count mismatch (wordid="UINT64_FMT"(%s), dict=%d, doclist=%d, hitlist=%d)",
				uint64_t(uWordid), sWord, iDictHits, iDoclistHits, iHitlistHits ));

		while ( m_bHaveSkips && iDoclistDocs>SPH_SKIPLIST_BLOCK )
		{
			if ( iSkipsOffset<=0 || iSkipsOffset>(int)m_pSkiplists.GetLengthBytes() )
			{
				LOC_FAIL(( fp, "invalid skiplist offset (wordid=%llu(%s), off=%d, max=%d)",
					UINT64 ( uWordid ), sWord, iSkipsOffset, (int)m_pSkiplists.GetLengthBytes() ));
				break;
			}

			// boundary adjustment
			if ( ( iDoclistDocs & ( SPH_SKIPLIST_BLOCK-1 ) )==0 )
				dDoclistSkips.Pop();

			SkiplistEntry_t t;
			t.m_iBaseDocid = m_iMinDocid;
			t.m_iOffset = iDoclistOffset;
			t.m_iBaseHitlistPos = 0;

			const BYTE * pSkip = m_pSkiplists.GetWritePtr() + iSkipsOffset;
			const BYTE * pMax = m_pSkiplists.GetWritePtr() + m_pSkiplists.GetLengthBytes();
			int i = 0;
			while ( pSkip<pMax && ++i<dDoclistSkips.GetLength() )
			{
				const SkiplistEntry_t & r = dDoclistSkips[i];
				t.m_iBaseDocid += SPH_SKIPLIST_BLOCK + (SphDocID_t) sphUnzipOffset ( pSkip );
				t.m_iOffset += 4*SPH_SKIPLIST_BLOCK + sphUnzipOffset ( pSkip );
				t.m_iBaseHitlistPos += sphUnzipOffset ( pSkip );
				if ( t.m_iBaseDocid!=r.m_iBaseDocid
					|| t.m_iOffset!=r.m_iOffset ||
					t.m_iBaseHitlistPos!=r.m_iBaseHitlistPos )
				{
					LOC_FAIL(( fp, "skiplist entry %d mismatch (wordid=%llu(%s), exp={%llu, %llu, %llu}, got={%llu, %llu, %llu})",
						i, UINT64 ( uWordid ), sWord,
						UINT64 ( r.m_iBaseDocid ), UINT64 ( r.m_iOffset ), UINT64 ( r.m_iBaseHitlistPos ),
						UINT64 ( t.m_iBaseDocid ), UINT64 ( t.m_iOffset ), UINT64 ( t.m_iBaseHitlistPos ) ));
					break;
				}
				if ( pSkip>pMax )
					LOC_FAIL(( fp, "skiplist length mismatch (wordid=%llu(%s), exp=%d, got=%d)",
						UINT64 ( uWordid ), sWord, i, dDoclistSkips.GetLength() ));
			}
			break;
		}

		// move my reader instance forward too
		rdDocs.SeekTo ( pQword->m_rdDoclist.GetPos(), READ_NO_SIZE_HINT );

		// cleanup
		SafeDelete ( pInlineStorage );
		SafeDelete ( pQword );

		// progress bar
		if ( (++iWordsChecked)%1000==0 && bProgress )
		{
			fprintf ( fp, "%d/%d\r", iWordsChecked, iWordsTotal );
			fflush ( fp );
		}
	}

	///////////////////////////
	// check rows (attributes)
	///////////////////////////

	if ( m_tSettings.m_eDocinfo==SPH_DOCINFO_EXTERN && !m_tAttr.IsEmpty() )
	{
		fprintf ( fp, "checking rows...\n" );

		// sizes and counts
		int64_t iRowsTotal = m_iDocinfo;
		DWORD uStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();

		int64_t iAllRowsTotal = iRowsTotal;
		iAllRowsTotal += (m_iDocinfoIndex+1)*2; // should had been fixed up to v.20 by the loader

		if ( iAllRowsTotal*uStride!=(int64_t)m_tAttr.GetNumEntries() )
			LOC_FAIL(( fp, "rowitems count mismatch (expected="INT64_FMT", loaded="INT64_FMT")",
				iAllRowsTotal*uStride, (int64_t)m_tAttr.GetNumEntries() ));

		// extract rowitem indexes for MVAs etc
		// (ie. attr types that we can and will run additional checks on)
		CSphVector<int> dMvaItems;
		CSphVector<CSphAttrLocator> dFloatItems;
		CSphVector<CSphAttrLocator> dStrItems;
		for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
		{
			const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
			if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET || tAttr.m_eAttrType==SPH_ATTR_INT64SET )
			{
				if ( tAttr.m_tLocator.m_iBitCount!=ROWITEM_BITS )
				{
					LOC_FAIL(( fp, "unexpected MVA bitcount (attr=%d, expected=%d, got=%d)",
						i, ROWITEM_BITS, tAttr.m_tLocator.m_iBitCount ));
					continue;
				}
				if ( ( tAttr.m_tLocator.m_iBitOffset % ROWITEM_BITS )!=0 )
				{
					LOC_FAIL(( fp, "unaligned MVA bitoffset (attr=%d, bitoffset=%d)",
						i, tAttr.m_tLocator.m_iBitOffset ));
					continue;
				}
				if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET )
				dMvaItems.Add ( tAttr.m_tLocator.m_iBitOffset/ROWITEM_BITS );
			} else if ( tAttr.m_eAttrType==SPH_ATTR_FLOAT )
				dFloatItems.Add	( tAttr.m_tLocator );
			else if ( tAttr.m_eAttrType==SPH_ATTR_STRING || tAttr.m_eAttrType==SPH_ATTR_JSON )
				dStrItems.Add ( tAttr.m_tLocator );
		}
		int iMva64 = dMvaItems.GetLength();
		for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
		{
			const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i);
			if ( tAttr.m_eAttrType==SPH_ATTR_INT64SET )
				dMvaItems.Add ( tAttr.m_tLocator.m_iBitOffset/ROWITEM_BITS );
		}

		// walk string data, build a list of acceptable start offsets
		// must be sorted by construction
		CSphVector<DWORD> dStringOffsets;
		if ( m_tString.GetNumEntries()>1 )
		{
			const BYTE * pBase = m_tString.GetWritePtr();
			const BYTE * pCur = pBase + 1;
			const BYTE * pMax = pBase + m_tString.GetNumEntries();
			while ( pCur<pMax )
			{
				const BYTE * pStr = NULL;
				const int iLen = sphUnpackStr ( pCur, &pStr );

				// 4 bytes must be enough to encode string length, hence pCur+4
				if ( pStr+iLen>pMax || pStr<pCur || pStr>pCur+4 )
				{
					LOC_FAIL(( fp, "string length out of bounds (offset=%u, len=%d)", (DWORD)(pCur-pBase), iLen ));
					break;
				}

				dStringOffsets.Add ( (DWORD)(pCur-pBase) );
				pCur = pStr + iLen;
			}
		}

		// loop the rows
		const CSphRowitem * pRow = m_tAttr.GetWritePtr();
		const DWORD * pMvaBase = m_tMva.GetWritePtr();
		const DWORD * pMvaMax = pMvaBase + m_tMva.GetNumEntries();
		const DWORD * pMva = pMvaBase;

		int iOrphan = 0;
		SphDocID_t uLastID = 0;

		for ( int64_t iRow=0; iRow<iRowsTotal; iRow++, pRow+=uStride )
		{
			// check that ids are ascending
			bool bIsSpaValid = uLastID < DOCINFO2ID(pRow);
			if ( !bIsSpaValid )
				LOC_FAIL(( fp, "docid decreased (row="INT64_FMT", id="DOCID_FMT", lastid="DOCID_FMT")",
					iRow, DOCINFO2ID(pRow), uLastID ));

			uLastID = DOCINFO2ID(pRow);

			///////////////////////////
			// check MVAs
			///////////////////////////

			if ( dMvaItems.GetLength() )
			{
				const DWORD * pMvaSpaFixed = NULL;
				const CSphRowitem * pAttrs = DOCINFO2ATTRS(pRow);
				bool bHasValues = false;
				ARRAY_FOREACH ( iItem, dMvaItems )
				{
					const DWORD uOffset = pAttrs[dMvaItems[iItem]];
					bHasValues |= uOffset!=0;

					if ( uOffset && pMvaBase+uOffset>=pMvaMax )
					{
						bIsSpaValid = false;
						LOC_FAIL(( fp, "MVA index out of bounds (row="INT64_FMT", mvaattr=%d, docid="DOCID_FMT", index=%u)",
							iRow, iItem, uLastID, uOffset ));
					}

					if ( uOffset && pMvaBase+uOffset<pMvaMax && !pMvaSpaFixed )
						pMvaSpaFixed = pMvaBase + uOffset - sizeof(SphDocID_t) / sizeof(DWORD);
				}

				// MVAs ptr recovery from previous errors only if current spa record is valid
				if ( pMva!=pMvaSpaFixed && bIsSpaValid && pMvaSpaFixed )
					pMva = pMvaSpaFixed;

				bool bLastIDChecked = false;

				SphDocID_t uLastMvaID = 0;
				while ( pMva<pMvaMax && DOCINFO2ID(pMva)<=uLastID )
				{
					const SphDocID_t uMvaID = DOCINFO2ID(pMva);
					pMva = DOCINFO2ATTRS(pMva);

					if ( bLastIDChecked && uLastID==uMvaID )
						LOC_FAIL(( fp, "duplicate docid found (row="INT64_FMT", docid expected="DOCID_FMT", got="DOCID_FMT", index=%u)",
							iRow, uLastID, uMvaID, (DWORD)(pMva-pMvaBase) ));

					if ( uMvaID<uLastMvaID )
						LOC_FAIL(( fp, "MVA docid decreased (row="INT64_FMT", spa docid="DOCID_FMT", last MVA docid="DOCID_FMT", MVA docid="DOCID_FMT", index=%u)",
							iRow, uLastID, uLastMvaID, uMvaID, (DWORD)(pMva-pMvaBase) ));

					bool bIsMvaCorrect = uLastMvaID<=uMvaID && uMvaID<=uLastID;
					uLastMvaID = uMvaID;

					// loop MVAs
					ARRAY_FOREACH_COND ( iItem, dMvaItems, bIsMvaCorrect )
					{
						const DWORD uSpaOffset = pAttrs[dMvaItems[iItem]];

						// zero offset means empty MVA in rt index
						if ( !uSpaOffset )
							continue;

						// check offset (index)
						if ( uMvaID==uLastID && bIsSpaValid && pMva!=pMvaBase+uSpaOffset )
						{
							LOC_FAIL(( fp, "unexpected MVA docid (row="INT64_FMT", mvaattr=%d, docid expected="DOCID_FMT", got="DOCID_FMT", expected=%u, got=%u)",
								iRow, iItem, uLastID, uMvaID, (DWORD)(pMva-pMvaBase), uSpaOffset ));
							// it's unexpected but it's our best guess
							// but do fix up only once, to prevent infinite loop
							if ( !bLastIDChecked )
								pMva = pMvaBase+uSpaOffset;
						}

						if ( pMva>=pMvaMax )
						{
							LOC_FAIL(( fp, "MVA index out of bounds (row="INT64_FMT", mvaattr=%d, docid expected="DOCID_FMT", got="DOCID_FMT", index=%u)",
								iRow, iItem, uLastID, uMvaID, (DWORD)(pMva-pMvaBase) ));
							bIsMvaCorrect = false;
							continue;
						}

						// check values
						DWORD uValues = *pMva++;

						if ( pMva+uValues-1>=pMvaMax )
						{
							LOC_FAIL(( fp, "MVA count out of bounds (row="INT64_FMT", mvaattr=%d, docid expected="DOCID_FMT", got="DOCID_FMT", count=%u)",
								iRow, iItem, uLastID, uMvaID, uValues ));
							pMva += uValues;
							bIsMvaCorrect = false;
							continue;
						}
						// check that values are ascending
						for ( DWORD uVal=(iItem>=iMva64 ? 2 : 1); uVal<uValues && bIsMvaCorrect; )
						{
							int64_t iPrev, iCur;
							if ( iItem>=iMva64 )
							{
								iPrev = MVA_UPSIZE ( pMva+uVal-2 );
								iCur = MVA_UPSIZE ( pMva+uVal );
								uVal += 2;
							} else
							{
								iPrev = pMva[uVal-1];
								iCur = pMva[uVal];
								uVal++;
							}

							if ( iCur<=iPrev )
							{
								LOC_FAIL(( fp, "unsorted MVA values (row="INT64_FMT", mvaattr=%d, docid expected="DOCID_FMT", got="DOCID_FMT", val[%u]=%u, val[%u]=%u)",
									iRow, iItem, uLastID, uMvaID, ( iItem>=iMva64 ? uVal-2 : uVal-1 ), (unsigned int)iPrev, uVal, (unsigned int)iCur ));
								bIsMvaCorrect = false;
							}

							uVal += ( iItem>=iMva64 ? 2 : 1 );
						}
						pMva += uValues;
					}

					if ( !bIsMvaCorrect )
						break;

					// orphan only ON no errors && ( not matched ids || ids matched multiply times )
					if ( bIsMvaCorrect && ( uMvaID!=uLastID || ( uMvaID==uLastID && bLastIDChecked ) ) )
						iOrphan++;

					bLastIDChecked |= uLastID==uMvaID;
				}

				if ( !bLastIDChecked && bHasValues )
					LOC_FAIL(( fp, "missed or damaged MVA (row="INT64_FMT", docid expected="DOCID_FMT")",
						iRow, uLastID ));
			}

			///////////////////////////
			// check floats
			///////////////////////////

			ARRAY_FOREACH ( iItem, dFloatItems )
			{
				const CSphRowitem * pAttrs = DOCINFO2ATTRS(pRow);
				const DWORD uValue = (DWORD)sphGetRowAttr ( pAttrs, dFloatItems[ iItem ] );
				const DWORD uExp = ( uValue >> 23 ) & 0xff;
				const DWORD uMantissa = uValue & 0x003fffff;

				// check normalized
				if ( uExp==0 && uMantissa!=0 )
					LOC_FAIL(( fp, "float attribute value is unnormalized (row="INT64_FMT", attr=%d, id="DOCID_FMT", raw=0x%x, value=%f)",
						iRow, iItem, uLastID, uValue, sphDW2F ( uValue ) ));

				// check +-inf
				if ( uExp==0xff && uMantissa==0 )
					LOC_FAIL(( fp, "float attribute is infinity (row="INT64_FMT", attr=%d, id="DOCID_FMT", raw=0x%x, value=%f)",
						iRow, iItem, uLastID, uValue, sphDW2F ( uValue ) ));
			}

			/////////////////
			// check strings
			/////////////////

			ARRAY_FOREACH ( iItem, dStrItems )
			{
				const CSphRowitem * pAttrs = DOCINFO2ATTRS(pRow);

				const DWORD uOffset = (DWORD)sphGetRowAttr ( pAttrs, dStrItems[ iItem ] );
				if ( uOffset>=m_tString.GetNumEntries() )
				{
					LOC_FAIL(( fp, "string offset out of bounds (row="INT64_FMT", stringattr=%d, docid="DOCID_FMT", index=%u)",
						iRow, iItem, uLastID, uOffset ));
					continue;
				}

				if ( !uOffset )
					continue;

				const BYTE * pStr = NULL;
				const int iLen = sphUnpackStr ( m_tString.GetWritePtr() + uOffset, &pStr );

				// check that length is sane
				if ( pStr+iLen-1>=m_tString.GetWritePtr()+m_tString.GetLengthBytes() )
				{
					LOC_FAIL(( fp, "string length out of bounds (row="INT64_FMT", stringattr=%d, docid="DOCID_FMT", index=%u)",
						iRow, iItem, uLastID, (unsigned int)( pStr-m_tString.GetWritePtr()+iLen-1 ) ));
					continue;
				}

				// check that offset is one of the good ones
				// (that is, that we don't point in the middle of some other data)
				if ( !dStringOffsets.BinarySearch ( uOffset ) )
				{
					LOC_FAIL(( fp, "string offset is not a string start (row="INT64_FMT", stringattr=%d, docid="DOCID_FMT", offset=%u)",
						iRow, iItem, uLastID, uOffset ));
				}
			}

			// progress bar
			if ( iRow%1000==0 && bProgress )
			{
				fprintf ( fp, INT64_FMT"/"INT64_FMT"\r", iRow, iRowsTotal );
				fflush ( fp );
			}
		}

		if ( iOrphan )
			fprintf ( fp, "WARNING: %d orphaned MVA entries were found\n", iOrphan );

		///////////////////////////
		// check blocks index
		///////////////////////////

		fprintf ( fp, "checking attribute blocks index...\n" );

		// check size
		const int64_t iTempDocinfoIndex = ( m_iDocinfo+DOCINFO_INDEX_FREQ-1 ) / DOCINFO_INDEX_FREQ;
		if ( iTempDocinfoIndex!=m_iDocinfoIndex )
			LOC_FAIL(( fp, "block count differs (expected="INT64_FMT", got="INT64_FMT")",
				iTempDocinfoIndex, m_iDocinfoIndex ));

		const DWORD uMinMaxStride = DOCINFO_IDSIZE + m_tSchema.GetRowSize();
		const DWORD * pDocinfoIndexMax = m_pDocinfoIndex + ( m_iDocinfoIndex+1 )*uMinMaxStride*2;

		for ( int64_t iIndexEntry=0; iIndexEntry<m_iDocinfo; iIndexEntry++ )
		{
			const int64_t iBlock = iIndexEntry / DOCINFO_INDEX_FREQ;

			// we have to do some checks in border cases, for example: when move from 1st to 2nd block
			const int64_t iPrevEntryBlock = ( iIndexEntry-1 )/DOCINFO_INDEX_FREQ;
			const bool bIsBordersCheckTime = ( iPrevEntryBlock!=iBlock );

			const DWORD * pAttr = m_tAttr.GetWritePtr() + iIndexEntry * uMinMaxStride;
			const SphDocID_t uDocID = DOCINFO2ID(pAttr);

			const DWORD * pMinEntry = m_pDocinfoIndex + iBlock * uMinMaxStride * 2;
			const DWORD * pMaxEntry = pMinEntry + uMinMaxStride;
			const DWORD * pMinAttrs = DOCINFO2ATTRS ( pMinEntry );
			const DWORD * pMaxAttrs = pMinAttrs + uMinMaxStride;

			// check docid vs global range
			if ( pMaxEntry+uMinMaxStride > pDocinfoIndexMax )
				LOC_FAIL(( fp, "unexpected block index end (row="INT64_FMT", docid="DOCID_FMT", block="INT64_FMT", max="INT64_FMT", cur="INT64_FMT")",
					iIndexEntry, uDocID, iBlock, int64_t ( pDocinfoIndexMax-m_pDocinfoIndex ), int64_t ( pMaxEntry+uMinMaxStride-m_pDocinfoIndex ) ));

			// check attribute location vs global range
			if ( pMaxAttrs+uMinMaxStride > pDocinfoIndexMax )
				LOC_FAIL(( fp, "attribute position out of blocks index (row="INT64_FMT", docid="DOCID_FMT", block="INT64_FMT", expected<"INT64_FMT", got="INT64_FMT")",
					iIndexEntry, uDocID, iBlock, int64_t ( pDocinfoIndexMax-m_pDocinfoIndex ), int64_t ( pMaxAttrs+uMinMaxStride-m_pDocinfoIndex ) ));

			const SphDocID_t uMinDocID = DOCINFO2ID ( pMinEntry );
			const SphDocID_t uMaxDocID = DOCINFO2ID ( pMaxEntry );

			// checks is docid min max range valid
			if ( uMinDocID > uMaxDocID && bIsBordersCheckTime )
				LOC_FAIL(( fp, "invalid docid range (row="INT64_FMT", block="INT64_FMT", min="DOCID_FMT", max="DOCID_FMT")",
					iIndexEntry, iBlock, uMinDocID, uMaxDocID ));

			// checks docid vs blocks range
			if ( uDocID < uMinDocID || uDocID > uMaxDocID )
				LOC_FAIL(( fp, "unexpected docid range (row="INT64_FMT", docid="DOCID_FMT", block="INT64_FMT", min="DOCID_FMT", max="DOCID_FMT")",
					iIndexEntry, uDocID, iBlock, uMinDocID, uMaxDocID ));

			bool bIsFirstMva = true;

			// check values vs blocks range
			const DWORD * pSpaRow = DOCINFO2ATTRS(pAttr);
			for ( int iItem=0; iItem<m_tSchema.GetAttrsCount(); iItem++ )
			{
				const CSphColumnInfo & tCol = m_tSchema.GetAttr(iItem);

				switch ( tCol.m_eAttrType )
				{
				case SPH_ATTR_INTEGER:
				case SPH_ATTR_TIMESTAMP:
				case SPH_ATTR_BOOL:
				case SPH_ATTR_BIGINT:
					{
						const SphAttr_t uVal = sphGetRowAttr ( pSpaRow, tCol.m_tLocator );
						const SphAttr_t uMin = sphGetRowAttr ( pMinAttrs, tCol.m_tLocator );
						const SphAttr_t uMax = sphGetRowAttr ( pMaxAttrs, tCol.m_tLocator );

						// checks is attribute min max range valid
						if ( uMin > uMax && bIsBordersCheckTime )
							LOC_FAIL(( fp, "invalid attribute range (row="INT64_FMT", block="INT64_FMT", min="INT64_FMT", max="INT64_FMT")",
								iIndexEntry, iBlock, uMin, uMax ));

						if ( uVal < uMin || uVal > uMax )
							LOC_FAIL(( fp, "unexpected attribute value (row="INT64_FMT", attr=%u, docid="DOCID_FMT", block="INT64_FMT", value=0x"UINT64_FMT", min=0x"UINT64_FMT", max=0x"UINT64_FMT")",
								iIndexEntry, iItem, uDocID, iBlock, uint64_t(uVal), uint64_t(uMin), uint64_t(uMax) ));
					}
					break;

				case SPH_ATTR_FLOAT:
					{
						const float fVal = sphDW2F ( (DWORD)sphGetRowAttr ( pSpaRow, tCol.m_tLocator ) );
						const float fMin = sphDW2F ( (DWORD)sphGetRowAttr ( pMinAttrs, tCol.m_tLocator ) );
						const float fMax = sphDW2F ( (DWORD)sphGetRowAttr ( pMaxAttrs, tCol.m_tLocator ) );

						// checks is attribute min max range valid
						if ( fMin > fMax && bIsBordersCheckTime )
							LOC_FAIL(( fp, "invalid attribute range (row="INT64_FMT", block="INT64_FMT", min=%f, max=%f)",
								iIndexEntry, iBlock, fMin, fMax ));

						if ( fVal < fMin || fVal > fMax )
							LOC_FAIL(( fp, "unexpected attribute value (row="INT64_FMT", attr=%u, docid="DOCID_FMT", block="INT64_FMT", value=%f, min=%f, max=%f)",
								iIndexEntry, iItem, uDocID, iBlock, fVal, fMin, fMax ));
					}
					break;

				case SPH_ATTR_UINT32SET:
					{
						const DWORD uMin = (DWORD)sphGetRowAttr ( pMinAttrs, tCol.m_tLocator );
						const DWORD uMax = (DWORD)sphGetRowAttr ( pMaxAttrs, tCol.m_tLocator );

						// checks is MVA attribute min max range valid
						if ( uMin > uMax && bIsBordersCheckTime && uMin!=0xffffffff && uMax!=0 )
							LOC_FAIL(( fp, "invalid MVA range (row="INT64_FMT", block="INT64_FMT", min=0x%x, max=0x%x)",
							iIndexEntry, iBlock, uMin, uMax ));

						SphAttr_t uOff = sphGetRowAttr ( pSpaRow, tCol.m_tLocator );
						if ( !uOff )
							break;

						pMva = m_tMva.GetWritePtr() + uOff;
						const DWORD * pMvaDocID = bIsFirstMva ? ( pMva - sizeof(SphDocID_t) / sizeof(DWORD) ) : NULL;
						bIsFirstMva = false;

						if ( uOff>=(SphAttr_t)m_tMva.GetNumEntries() )
							break;

						if ( pMvaDocID && DOCINFO2ID ( pMvaDocID )!=uDocID )
						{
							LOC_FAIL(( fp, "unexpected MVA docid (row="INT64_FMT", mvaattr=%d, expected="DOCID_FMT", got="DOCID_FMT", block="INT64_FMT", index=%u)",
								iIndexEntry, iItem, uDocID, DOCINFO2ID ( pMvaDocID ), iBlock, (DWORD)uOff ));
							break;
						}

						// check values
						const DWORD uValues = *pMva++;
						if ( uOff+uValues>(SphAttr_t)m_tMva.GetNumEntries() )
							break;

						for ( DWORD iVal=0; iVal<uValues; iVal++ )
						{
							const DWORD uVal = *pMva++;
							if ( uVal < uMin || uVal > uMax )
								LOC_FAIL(( fp, "unexpected MVA value (row="INT64_FMT", attr=%u, docid="DOCID_FMT", block="INT64_FMT", index=%u, value=0x%x, min=0x%x, max=0x%x)",
									iIndexEntry, iItem, uDocID, iBlock, iVal, (DWORD)uVal, (DWORD)uMin, (DWORD)uMax ));
						}
					}
					break;

				default:
					break;
				}
			}

			// progress bar
			if ( iIndexEntry%1000==0 && bProgress )
			{
				fprintf ( fp, INT64_FMT"/"INT64_FMT"\r", iIndexEntry, m_iDocinfo );
				fflush ( fp );
			}
		}
	}

	///////////////////////////
	// check kill-list
	///////////////////////////

	fprintf ( fp, "checking kill-list...\n" );

	// check size
	if ( m_pKillList.GetNumEntries()!=m_iKillListSize )
		LOC_FAIL(( fp, "kill-list size differs (expected=%d, got="INT64_FMT")",
			m_iKillListSize, (int64_t)m_pKillList.GetNumEntries() ));

	// check that ids are ascending
	for ( DWORD uID=1; uID<m_pKillList.GetNumEntries(); uID++ )
		if ( m_pKillList[uID]<=m_pKillList[uID-1] )
			LOC_FAIL(( fp, "unsorted kill-list values (val[%d]=%d, val[%d]=%d)",
				uID-1, (DWORD)m_pKillList[uID-1], uID, (DWORD)m_pKillList[uID] ));

	///////////////////////////
	// all finished
	///////////////////////////

	// well, no known kinds of failures, maybe some unknown ones
	tmCheck = sphMicroTimer() - tmCheck;
	if ( !iFails )
		fprintf ( fp, "check passed" );
	else if ( iFails!=iFailsPrinted )
		fprintf ( fp, "check FAILED, %d of "INT64_FMT" failures reported", iFailsPrinted, iFails );
	else
		fprintf ( fp, "check FAILED, "INT64_FMT" failures reported", iFails );
	fprintf ( fp, ", %d.%d sec elapsed\n", (int)(tmCheck/1000000), (int)((tmCheck/100000)%10) );

	return (int)Min ( iFails, 255 ); // this is the exitcode; so cap it
} // NOLINT function length


//////////////////////////////////////////////////////////////////////////

/// morphology
enum
{
	SPH_MORPH_STEM_EN,
	SPH_MORPH_STEM_RU_CP1251,
	SPH_MORPH_STEM_RU_UTF8,
	SPH_MORPH_STEM_CZ,
	SPH_MORPH_STEM_AR_UTF8,
	SPH_MORPH_SOUNDEX,
	SPH_MORPH_METAPHONE_SBCS,
	SPH_MORPH_METAPHONE_UTF8,
	SPH_MORPH_AOTLEMMER_RU_CP1251,
	SPH_MORPH_AOTLEMMER_BASE,
	SPH_MORPH_AOTLEMMER_RU_UTF8 = SPH_MORPH_AOTLEMMER_BASE,
	SPH_MORPH_AOTLEMMER_EN,
	SPH_MORPH_AOTLEMMER_DE_CP1252,
	SPH_MORPH_AOTLEMMER_DE_UTF8,
	SPH_MORPH_AOTLEMMER_BASE_ALL,
	SPH_MORPH_AOTLEMMER_RU_ALL = SPH_MORPH_AOTLEMMER_BASE_ALL,
	SPH_MORPH_AOTLEMMER_EN_ALL,
	SPH_MORPH_AOTLEMMER_DE_ALL,
	SPH_MORPH_LIBSTEMMER_FIRST,
	SPH_MORPH_LIBSTEMMER_LAST = SPH_MORPH_LIBSTEMMER_FIRST + 64
};


/////////////////////////////////////////////////////////////////////////////
// BASE DICTIONARY INTERFACE
/////////////////////////////////////////////////////////////////////////////

void CSphDict::DictBegin ( CSphAutofile &, CSphAutofile &, int, ThrottleState_t * )		{}
void CSphDict::DictEntry ( const CSphDictEntry & )										{}
void CSphDict::DictEndEntries ( SphOffset_t )											{}
bool CSphDict::DictEnd ( DictHeader_t *, int, CSphString &, ThrottleState_t * )			{ return true; }
bool CSphDict::DictIsError () const														{ return true; }

/////////////////////////////////////////////////////////////////////////////
// CRC32/64 DICTIONARIES
/////////////////////////////////////////////////////////////////////////////

struct CSphTemplateDictTraits : CSphDict
{
	CSphTemplateDictTraits ();
	virtual				~CSphTemplateDictTraits ();

	virtual void		LoadStopwords ( const char * sFiles, const ISphTokenizer * pTokenizer );
	virtual void		LoadStopwords ( const CSphVector<SphWordID_t> & dStopwords );
	virtual void		WriteStopwords ( CSphWriter & tWriter );
	virtual bool		LoadWordforms ( const CSphVector<CSphString> & dFiles, const CSphEmbeddedFiles * pEmbedded, const ISphTokenizer * pTokenizer, const char * sIndex );
	virtual void		WriteWordforms ( CSphWriter & tWriter );
	virtual const CSphWordforms *	GetWordforms() { return m_pWordforms; }
	virtual void		DisableWordforms() { m_bDisableWordforms = true; }
	virtual int			SetMorphology ( const char * szMorph, bool bUseUTF8, CSphString & sMessage );
	virtual bool		HasMorphology() const;
	virtual void		ApplyStemmers ( BYTE * pWord );

	virtual void		Setup ( const CSphDictSettings & tSettings ) { m_tSettings = tSettings; }
	virtual const CSphDictSettings & GetSettings () const { return m_tSettings; }
	virtual const CSphVector <CSphSavedFile> & GetStopwordsFileInfos () { return m_dSWFileInfos; }
	virtual const CSphVector <CSphSavedFile> & GetWordformsFileInfos () { return m_dWFFileInfos; }
	virtual const CSphMultiformContainer * GetMultiWordforms () const;
	virtual uint64_t	GetSettingsFNV () const;
	virtual void		SetApplyMorph ( bool bApply ) { m_bApplyMorph = bApply; }
	static void			SweepWordformContainers ( const CSphVector<CSphSavedFile> & dFiles );

protected:
	CSphVector < int >	m_dMorph;
#if USE_LIBSTEMMER
	CSphVector < sb_stemmer * >	m_dStemmers;
	struct DescStemmer_t
	{
		CSphString m_sAlgo;
		CSphString m_sEnc;
	};
	CSphVector<DescStemmer_t> m_dDescStemmers;
#endif

	int					m_iStopwords;	///< stopwords count
	SphWordID_t *		m_pStopwords;	///< stopwords ID list
	CSphFixedVector<SphWordID_t> m_dStopwordContainer;
	bool				m_bApplyMorph;

protected:
	int					ParseMorphology ( const char * szMorph, bool bUseUTF8, CSphString & sError );
	SphWordID_t			FilterStopword ( SphWordID_t uID ) const;	///< filter ID against stopwords list
	CSphDict *			CloneBase ( CSphTemplateDictTraits * pDict ) const;
	virtual bool		HasState () const;

	bool				m_bDisableWordforms;

private:
	CSphWordforms *				m_pWordforms;
	CSphVector<CSphSavedFile>	m_dSWFileInfos;
	CSphVector<CSphSavedFile>	m_dWFFileInfos;
	CSphDictSettings			m_tSettings;

	static CSphVector<CSphWordforms*>		m_dWordformContainers;

	CSphWordforms *		GetWordformContainer ( const CSphVector<CSphSavedFile> & dFileInfos, const CSphVector<CSphString> * pEmbeddedWordforms, const ISphTokenizer * pTokenizer, const char * sIndex );
	CSphWordforms *		LoadWordformContainer ( const CSphVector<CSphSavedFile> & dFileInfos, const CSphVector<CSphString> * pEmbeddedWordforms, const ISphTokenizer * pTokenizer, const char * sIndex );

	int					InitMorph ( const char * szMorph, int iLength, bool bUseUTF8, CSphString & sError );
	int					AddMorph ( int iMorph ); ///< helper that always returns ST_OK
	bool				StemById ( BYTE * pWord, int iStemmer );
	void				AddWordform ( CSphWordforms * pContainer, char * sBuffer, int iLen, ISphTokenizer * pTokenizer, const char * szFile, const CSphVector<char> & dBlended );
};

CSphVector<CSphWordforms*> CSphTemplateDictTraits::m_dWordformContainers;


/// common CRC32/64 dictionary stuff
struct CSphDiskDictTraits : CSphTemplateDictTraits
{
						CSphDiskDictTraits ()
							: m_iEntries ( 0 )
							, m_iLastDoclistPos ( 0 )
							, m_iLastWordID ( 0 )
						{}
						virtual				~CSphDiskDictTraits () {}

	virtual void DictBegin ( CSphAutofile & tTempDict, CSphAutofile & tDict, int iDictLimit, ThrottleState_t * pThrottle );
	virtual void DictEntry ( const CSphDictEntry & tEntry );
	virtual void DictEndEntries ( SphOffset_t iDoclistOffset );
	virtual bool DictEnd ( DictHeader_t * pHeader, int iMemLimit, CSphString & sError, ThrottleState_t * );
	virtual bool DictIsError () const { return m_wrDict.IsError(); }

protected:

	CSphTightVector<CSphWordlistCheckpoint>	m_dCheckpoints;		///< checkpoint offsets
	CSphWriter			m_wrDict;			///< final dict file writer
	CSphString			m_sWriterError;		///< writer error message storage
	int					m_iEntries;			///< dictionary entries stored
	SphOffset_t			m_iLastDoclistPos;
	SphWordID_t			m_iLastWordID;
};


template < bool CRCALGO >
struct CCRCEngine
{
	inline static SphWordID_t		DoCrc ( const BYTE * pWord );
	inline static SphWordID_t		DoCrc ( const BYTE * pWord, int iLen );
};

/// specialized CRC32/64 implementations
template < bool CRC32DICT >
struct CSphDictCRC : public CSphDiskDictTraits, CCRCEngine<CRC32DICT>
{
	typedef CCRCEngine<CRC32DICT> tHASH;
	virtual SphWordID_t		GetWordID ( BYTE * pWord );
	virtual SphWordID_t		GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops );
	virtual SphWordID_t		GetWordIDWithMarkers ( BYTE * pWord );
	virtual SphWordID_t		GetWordIDNonStemmed ( BYTE * pWord );
	virtual bool			IsStopWord ( const BYTE * pWord ) const;

	virtual CSphDict *		Clone () const { return CloneBase ( new CSphDictCRC<CRC32DICT>() ); }
};

struct CSphDictTemplate : public CSphTemplateDictTraits, CCRCEngine<false> // based on flv64
{
	virtual SphWordID_t		GetWordID ( BYTE * pWord );
	virtual SphWordID_t		GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops );
	virtual SphWordID_t		GetWordIDWithMarkers ( BYTE * pWord );
	virtual SphWordID_t		GetWordIDNonStemmed ( BYTE * pWord );
	virtual bool			IsStopWord ( const BYTE * pWord ) const;

	virtual CSphDict *		Clone () const { return CloneBase ( new CSphDictTemplate() ); }
};


/////////////////////////////////////////////////////////////////////////////

uint64_t sphFNV64 ( const BYTE * s )
{
	return sphFNV64cont ( s, SPH_FNV64_SEED );
}


uint64_t sphFNV64 ( const BYTE * s, int iLen, uint64_t uPrev )
{
	uint64_t hval = uPrev;
	for ( ; iLen>0; iLen-- )
	{
		// xor the bottom with the current octet
		hval ^= (uint64_t)*s++;

		// multiply by the 64 bit FNV magic prime mod 2^64
		hval += (hval << 1) + (hval << 4) + (hval << 5) + (hval << 7) + (hval << 8) + (hval << 40); // gcc optimization
	}
	return hval;
}


uint64_t sphFNV64cont ( const BYTE * s, uint64_t uPrev )
{
	uint64_t hval = uPrev;
	while ( *s )
	{
		// xor the bottom with the current octet
		hval ^= (uint64_t)*s++;

		// multiply by the 64 bit FNV magic prime mod 2^64
		hval += (hval << 1) + (hval << 4) + (hval << 5) + (hval << 7) + (hval << 8) + (hval << 40); // gcc optimization
	}
	return hval;
}

/////////////////////////////////////////////////////////////////////////////

extern DWORD g_dSphinxCRC32 [ 256 ];

bool sphCalcFileCRC32 ( const char * szFilename, DWORD & uCRC32 )
{
	uCRC32 = 0;

	if ( !szFilename )
		return false;

	FILE * pFile = fopen ( szFilename, "rb" );
	if ( !pFile )
		return false;

	DWORD crc = ~((DWORD)0);

	const int BUFFER_SIZE = 131072;
	static BYTE * pBuffer = NULL;
	if ( !pBuffer )
		pBuffer = new BYTE [ BUFFER_SIZE ];

	int iBytesRead;
	while ( ( iBytesRead = fread ( pBuffer, 1, BUFFER_SIZE, pFile ) )!=0 )
	{
		for ( int i=0; i<iBytesRead; i++ )
			crc = (crc >> 8) ^ g_dSphinxCRC32 [ (crc ^ pBuffer[i]) & 0xff ];
	}

	fclose ( pFile );

	uCRC32 = ~crc;
	return true;
}


static bool GetFileStats ( const char * szFilename, CSphSavedFile & tInfo, CSphString * pError )
{
	if ( !szFilename || !*szFilename )
	{
		memset ( &tInfo, 0, sizeof ( tInfo ) );
		return true;
	}

	tInfo.m_sFilename = szFilename;

	struct_stat tStat;
	memset ( &tStat, 0, sizeof ( tStat ) );
	if ( stat ( szFilename, &tStat ) < 0 )
	{
		if ( pError )
			*pError = strerror ( errno );
		memset ( &tStat, 0, sizeof ( tStat ) );
		return false;
	}

	tInfo.m_uSize = tStat.st_size;
	tInfo.m_uCTime = tStat.st_ctime;
	tInfo.m_uMTime = tStat.st_mtime;

	DWORD uCRC32 = 0;
	if ( !sphCalcFileCRC32 ( szFilename, uCRC32 ) )
		return false;

	tInfo.m_uCRC32 = uCRC32;

	return true;
}

/////////////////////////////////////////////////////////////////////////////

CSphWordforms::CSphWordforms()
	: m_iRefCount ( 0 )
	, m_uTokenizerFNV ( 0 )
	, m_bHavePostMorphNF ( false )
	, m_pMultiWordforms ( NULL )
{
}


CSphWordforms::~CSphWordforms()
{
	if ( m_pMultiWordforms )
	{
		m_pMultiWordforms->m_Hash.IterateStart ();
		while ( m_pMultiWordforms->m_Hash.IterateNext () )
		{
			CSphMultiforms * pWordforms = m_pMultiWordforms->m_Hash.IterateGet ();
			ARRAY_FOREACH ( i, pWordforms->m_pForms )
				SafeDelete ( pWordforms->m_pForms[i] );

			SafeDelete ( pWordforms );
		}

		SafeDelete ( m_pMultiWordforms );
	}
}


bool CSphWordforms::IsEqual ( const CSphVector<CSphSavedFile> & dFiles )
{
	if ( m_dFiles.GetLength()!=dFiles.GetLength() )
		return false;

	ARRAY_FOREACH ( i, m_dFiles )
	{
		const CSphSavedFile & tF1 = m_dFiles[i];
		const CSphSavedFile & tF2 = dFiles[i];
		if ( tF1.m_sFilename!=tF2.m_sFilename || tF1.m_uCRC32!=tF2.m_uCRC32 || tF1.m_uSize!=tF2.m_uSize ||
			tF1.m_uCTime!=tF2.m_uCTime || tF1.m_uMTime!=tF2.m_uMTime )
			return false;
	}

	return true;
}


bool CSphWordforms::ToNormalForm ( BYTE * pWord, bool bBefore ) const
{
	int * pIndex = m_dHash ( (char *)pWord );
	if ( !pIndex )
		return false;

	if ( *pIndex<0 || *pIndex>=m_dNormalForms.GetLength () )
		return false;

	if ( bBefore==m_dNormalForms[*pIndex].m_bAfterMorphology )
		return false;

	if ( m_dNormalForms[*pIndex].m_sWord.IsEmpty () )
		return false;

	strcpy ( (char *)pWord, m_dNormalForms[*pIndex].m_sWord.cstr() ); // NOLINT
	return true;
}

/////////////////////////////////////////////////////////////////////////////

CSphTemplateDictTraits::CSphTemplateDictTraits ()
	: m_iStopwords	( 0 )
	, m_pStopwords	( NULL )
	, m_dStopwordContainer ( 0 )
	, m_bApplyMorph ( true )
	, m_bDisableWordforms ( false )
	, m_pWordforms	( NULL )
{
}


CSphTemplateDictTraits::~CSphTemplateDictTraits ()
{
#if USE_LIBSTEMMER
	ARRAY_FOREACH ( i, m_dStemmers )
		sb_stemmer_delete ( m_dStemmers[i] );
#endif

	if ( m_pWordforms )
		--m_pWordforms->m_iRefCount;
}


SphWordID_t CSphTemplateDictTraits::FilterStopword ( SphWordID_t uID ) const
{
	if ( !m_iStopwords )
		return uID;

	// OPTIMIZE: binary search is not too good, could do some hashing instead
	SphWordID_t * pStart = m_pStopwords;
	SphWordID_t * pEnd = m_pStopwords + m_iStopwords - 1;
	do
	{
		if ( uID==*pStart || uID==*pEnd )
			return 0;

		if ( uID<*pStart || uID>*pEnd )
			return uID;

		SphWordID_t * pMid = pStart + (pEnd-pStart)/2;
		if ( uID==*pMid )
			return 0;

		if ( uID<*pMid )
			pEnd = pMid;
		else
			pStart = pMid;
	} while ( pEnd-pStart>1 );

	return uID;
}


int CSphTemplateDictTraits::ParseMorphology ( const char * sMorph, bool bUseUTF8, CSphString & sMessage )
{
	int iRes = ST_OK;
	for ( const char * sStart=sMorph; ; )
	{
		while ( *sStart && ( sphIsSpace ( *sStart ) || *sStart==',' ) )
			++sStart;
		if ( !*sStart )
			break;

		const char * sWordStart = sStart;
		while ( *sStart && !sphIsSpace ( *sStart ) && *sStart!=',' )
			++sStart;

		if ( sStart > sWordStart )
		{
			switch ( InitMorph ( sWordStart, sStart - sWordStart, bUseUTF8, sMessage ) )
			{
				case ST_ERROR:		return ST_ERROR;
				case ST_WARNING:	iRes = ST_WARNING;
				default:			break;
			}
		}
	}
	return iRes;
}


int CSphTemplateDictTraits::InitMorph ( const char * szMorph, int iLength, bool bUseUTF8, CSphString & sMessage )
{
	if ( iLength==0 )
		return ST_OK;

	if ( iLength==4 && !strncmp ( szMorph, "none", iLength ) )
		return ST_OK;

	if ( iLength==7 && !strncmp ( szMorph, "stem_en", iLength ) )
	{
		if ( m_dMorph.Contains ( SPH_MORPH_AOTLEMMER_EN ) )
		{
			sMessage.SetSprintf ( "stem_en and lemmatize_en clash" );
			return ST_ERROR;
		}

		if ( m_dMorph.Contains ( SPH_MORPH_AOTLEMMER_EN_ALL ) )
		{
			sMessage.SetSprintf ( "stem_en and lemmatize_en_all clash" );
			return ST_ERROR;
		}

		stem_en_init ();
		return AddMorph ( SPH_MORPH_STEM_EN );
	}

	if ( iLength==7 && !strncmp ( szMorph, "stem_ru", iLength ) )
	{
		if ( m_dMorph.Contains ( SPH_MORPH_AOTLEMMER_RU_CP1251 ) || m_dMorph.Contains ( SPH_MORPH_AOTLEMMER_RU_UTF8 ) )
		{
			sMessage.SetSprintf ( "stem_ru and lemmatize_ru clash" );
			return ST_ERROR;
		}

		if ( m_dMorph.Contains ( SPH_MORPH_AOTLEMMER_RU_ALL ) )
		{
			sMessage.SetSprintf ( "stem_ru and lemmatize_ru_all clash" );
			return ST_ERROR;
		}

		stem_ru_init ();
		return AddMorph ( bUseUTF8 ? SPH_MORPH_STEM_RU_UTF8 : SPH_MORPH_STEM_RU_CP1251 );
	}

	for ( int j=0; j<AOT_LENGTH; ++j )
	{
		char buf[20];
		char buf_all[20];
		sprintf ( buf, "lemmatize_%s", AOT_LANGUAGES[j] ); // NOLINT
		sprintf ( buf_all, "lemmatize_%s_all", AOT_LANGUAGES[j] ); // NOLINT

		if ( iLength==12 && !strncmp ( szMorph, buf, iLength ) )
		{
			if ( j==AOT_RU && ( m_dMorph.Contains ( SPH_MORPH_STEM_RU_CP1251 ) || m_dMorph.Contains ( SPH_MORPH_STEM_RU_UTF8 ) ) )
			{
				sMessage.SetSprintf ( "stem_ru and lemmatize_ru clash" );
				return ST_ERROR;
			}

			if ( j==AOT_EN && m_dMorph.Contains ( SPH_MORPH_STEM_EN ) )
			{
				sMessage.SetSprintf ( "stem_en and lemmatize_en clash" );
				return ST_ERROR;
			}

			// no test for SPH_MORPH_STEM_DE since we doesn't have it.

			if ( m_dMorph.Contains ( SPH_MORPH_AOTLEMMER_BASE_ALL+j ) )
			{
				sMessage.SetSprintf ( "%s and %s clash", buf, buf_all );
				return ST_ERROR;
			}

			CSphString sDictFile;
			sDictFile.SetSprintf ( "%s/%s.pak", g_sLemmatizerBase.cstr(), AOT_LANGUAGES[j] );
			if ( !sphAotInit ( sDictFile, sMessage, j ) )
				return ST_ERROR;

			// add manually instead of AddMorph(), because we need to update that fingerprint
			int iMorph = j + SPH_MORPH_AOTLEMMER_BASE;
			if ( j==AOT_RU )
				iMorph = bUseUTF8 ? SPH_MORPH_AOTLEMMER_RU_UTF8 : SPH_MORPH_AOTLEMMER_RU_CP1251;
			else if ( j==AOT_DE )
				iMorph = bUseUTF8 ? SPH_MORPH_AOTLEMMER_DE_UTF8 : SPH_MORPH_AOTLEMMER_DE_CP1252;

			if ( !m_dMorph.Contains ( iMorph ) )
			{
				if ( m_sMorphFingerprint.IsEmpty() )
					m_sMorphFingerprint.SetSprintf ( "%s:%08x"
						, sphAotDictinfo(j).m_sName.cstr()
						, sphAotDictinfo(j).m_iValue );
				else
					m_sMorphFingerprint.SetSprintf ( "%s;%s:%08x"
					, m_sMorphFingerprint.cstr()
					, sphAotDictinfo(j).m_sName.cstr()
					, sphAotDictinfo(j).m_iValue );
				m_dMorph.Add ( iMorph );
			}
			return ST_OK;
		}

		if ( iLength==16 && !strncmp ( szMorph, buf_all, iLength ) )
		{
			if ( j==AOT_RU && ( m_dMorph.Contains ( SPH_MORPH_STEM_RU_CP1251 ) || m_dMorph.Contains ( SPH_MORPH_STEM_RU_UTF8 ) ) )
			{
				sMessage.SetSprintf ( "stem_ru and lemmatize_ru_all clash" );
				return ST_ERROR;
			}

			if ( m_dMorph.Contains ( SPH_MORPH_AOTLEMMER_BASE+j ) )
			{
				sMessage.SetSprintf ( "%s and %s clash", buf, buf_all );
				return ST_ERROR;
			}

			CSphString sDictFile;
			sDictFile.SetSprintf ( "%s/%s.pak", g_sLemmatizerBase.cstr(), AOT_LANGUAGES[j] );
			if ( !sphAotInit ( sDictFile, sMessage, j ) )
				return ST_ERROR;

			return AddMorph ( SPH_MORPH_AOTLEMMER_BASE_ALL+j );
		}
	}

	if ( iLength==7 && !strncmp ( szMorph, "stem_cz", iLength ) )
	{
		stem_cz_init ();
		return AddMorph ( SPH_MORPH_STEM_CZ );
	}

	if ( iLength==7 && !strncmp ( szMorph, "stem_ar", iLength ) )
	{
		if ( !bUseUTF8 )
		{
			sMessage.SetSprintf ( "stem_ar only supports charset_type = utf-8" );
			return ST_ERROR;
		}
		return AddMorph ( SPH_MORPH_STEM_AR_UTF8 );
	}

	if ( iLength==9 && !strncmp ( szMorph, "stem_enru", iLength ) )
	{
		stem_en_init ();
		stem_ru_init ();
		AddMorph ( SPH_MORPH_STEM_EN );
		return AddMorph ( bUseUTF8 ? SPH_MORPH_STEM_RU_UTF8 : SPH_MORPH_STEM_RU_CP1251 );
	}

	if ( iLength==7 && !strncmp ( szMorph, "soundex", iLength ) )
		return AddMorph ( SPH_MORPH_SOUNDEX );

	if ( iLength==9 && !strncmp ( szMorph, "metaphone", iLength ) )
		return AddMorph ( bUseUTF8 ? SPH_MORPH_METAPHONE_UTF8 : SPH_MORPH_METAPHONE_SBCS );

#if USE_LIBSTEMMER
	const int LIBSTEMMER_LEN = 11;
	const int MAX_ALGO_LENGTH = 64;
	if ( iLength > LIBSTEMMER_LEN && iLength - LIBSTEMMER_LEN < MAX_ALGO_LENGTH && !strncmp ( szMorph, "libstemmer_", LIBSTEMMER_LEN ) )
	{
		CSphString sAlgo;
		CSphString sEnc;
		sAlgo.SetBinary ( szMorph+LIBSTEMMER_LEN, iLength - LIBSTEMMER_LEN );

		sb_stemmer * pStemmer = NULL;

		if ( bUseUTF8 )
		{
			sEnc = "UTF_8";
			pStemmer = sb_stemmer_new ( sAlgo.cstr(), sEnc.cstr() );
		} else
		{
			sEnc = "ISO_8859_1";
			pStemmer = sb_stemmer_new ( sAlgo.cstr(), sEnc.cstr() );

			if ( !pStemmer )
			{
				sEnc = "ISO_8859_2";
				pStemmer = sb_stemmer_new ( sAlgo.cstr(), sEnc.cstr() );
			}

			if ( !pStemmer )
			{
				sEnc = "KOI8_R";
				pStemmer = sb_stemmer_new ( sAlgo.cstr(), sEnc.cstr() );
			}
		}

		if ( !pStemmer )
		{
			sMessage.SetSprintf ( "unknown %s stemmer libstemmer_%s; skipped",
				bUseUTF8 ? "UTF-8" : "SBCS", sAlgo.cstr() );
			return ST_WARNING;
		}

		AddMorph ( SPH_MORPH_LIBSTEMMER_FIRST + m_dStemmers.GetLength () );
		ARRAY_FOREACH ( i, m_dStemmers )
		{
			if ( m_dStemmers[i]==pStemmer )
			{
				sb_stemmer_delete ( pStemmer );
				return ST_OK;
			}
		}

		m_dStemmers.Add ( pStemmer );
		DescStemmer_t & tDesc = m_dDescStemmers.Add();
		tDesc.m_sAlgo.Swap ( sAlgo );
		tDesc.m_sEnc.Swap ( sEnc );
		return ST_OK;
	}
#endif

	if ( iLength==11 && !strncmp ( szMorph, "rlp_chinese", iLength ) )
		return ST_OK;

	if ( iLength==19 && !strncmp ( szMorph, "rlp_chinese_batched", iLength ) )
		return ST_OK;

	sMessage.SetBinary ( szMorph, iLength );
	sMessage.SetSprintf ( "unknown stemmer %s; skipped", sMessage.cstr() );
	return ST_WARNING;
}


int CSphTemplateDictTraits::AddMorph ( int iMorph )
{
	if ( !m_dMorph.Contains ( iMorph ) )
		m_dMorph.Add ( iMorph );
	return ST_OK;
}



void CSphTemplateDictTraits::ApplyStemmers ( BYTE * pWord )
{
	if ( !m_bApplyMorph )
		return;

	// try wordforms
	if ( !m_bDisableWordforms && m_pWordforms && m_pWordforms->ToNormalForm ( pWord, true ) )
		return;

	// check length
	if ( m_tSettings.m_iMinStemmingLen<=1 || sphUTF8Len ( (const char*)pWord )>=m_tSettings.m_iMinStemmingLen )
	{
		// try stemmers
		ARRAY_FOREACH ( i, m_dMorph )
			if ( StemById ( pWord, m_dMorph[i] ) )
				break;
	}

	if ( !m_bDisableWordforms && m_pWordforms && m_pWordforms->m_bHavePostMorphNF )
		m_pWordforms->ToNormalForm ( pWord, false );
}

const CSphMultiformContainer * CSphTemplateDictTraits::GetMultiWordforms () const
{
	return m_pWordforms ? m_pWordforms->m_pMultiWordforms : NULL;
}

uint64_t CSphTemplateDictTraits::GetSettingsFNV () const
{
	uint64_t uHash = (uint64_t)m_pWordforms;

	if ( m_pStopwords )
		uHash = sphFNV64 ( (const BYTE *)m_pStopwords, m_iStopwords*sizeof(*m_pStopwords), uHash );

	uHash = sphFNV64 ( (const BYTE *)&m_tSettings.m_iMinStemmingLen, sizeof(m_tSettings.m_iMinStemmingLen), uHash );
	DWORD uFlags = 0;
	if ( m_tSettings.m_bWordDict )
		uFlags |= 1<<0;
	if ( m_tSettings.m_bCrc32 )
		uFlags |= 1<<1;
	if ( m_tSettings.m_bStopwordsUnstemmed )
		uFlags |= 1<<2;
	uHash = sphFNV64 ( (const BYTE *)&uFlags, sizeof(uFlags), uHash );

	uHash = sphFNV64 ( (const BYTE *)m_dMorph.Begin(), m_dMorph.GetLength()*sizeof(m_dMorph[0]), uHash );
#if USE_LIBSTEMMER
	ARRAY_FOREACH ( i, m_dDescStemmers )
	{
		uHash = sphFNV64 ( (const BYTE *)m_dDescStemmers[i].m_sAlgo.cstr(), m_dDescStemmers[i].m_sAlgo.Length(), uHash );
		uHash = sphFNV64 ( (const BYTE *)m_dDescStemmers[i].m_sEnc.cstr(), m_dDescStemmers[i].m_sEnc.Length(), uHash );
	}
#endif

	return uHash;
}

CSphDict * CSphTemplateDictTraits::CloneBase ( CSphTemplateDictTraits * pDict ) const
{
	assert ( pDict );
	pDict->m_tSettings = m_tSettings;
	pDict->m_iStopwords = m_iStopwords;
	pDict->m_pStopwords = m_pStopwords;
	pDict->m_dSWFileInfos = m_dSWFileInfos;
	pDict->m_dWFFileInfos = m_dWFFileInfos;
	pDict->m_pWordforms = m_pWordforms;
	if ( m_pWordforms )
		m_pWordforms->m_iRefCount++;

	pDict->m_dMorph = m_dMorph;
#if USE_LIBSTEMMER
	assert ( m_dDescStemmers.GetLength()==m_dStemmers.GetLength() );
	pDict->m_dDescStemmers = m_dDescStemmers;
	ARRAY_FOREACH ( i, m_dDescStemmers )
	{
		pDict->m_dStemmers.Add ( sb_stemmer_new ( m_dDescStemmers[i].m_sAlgo.cstr(), m_dDescStemmers[i].m_sEnc.cstr() ) );
		assert ( pDict->m_dStemmers.Last() );
	}
#endif

	pDict->m_bApplyMorph = m_bApplyMorph;

	return pDict;
}

bool CSphTemplateDictTraits::HasState() const
{
#if USE_RLP
	return true;
#else
#if !USE_LIBSTEMMER
	return false;
#else
	return ( m_dDescStemmers.GetLength()>0 );
#endif
#endif
}

/////////////////////////////////////////////////////////////////////////////

template<>
SphWordID_t CCRCEngine<true>::DoCrc ( const BYTE * pWord )
{
	return sphCRC32 ( pWord );
}


template<>
SphWordID_t CCRCEngine<false>::DoCrc ( const BYTE * pWord )
{
	return (SphWordID_t) sphFNV64 ( pWord );
}


template<>
SphWordID_t CCRCEngine<true>::DoCrc ( const BYTE * pWord, int iLen )
{
	return sphCRC32 ( pWord, iLen );
}


template<>
SphWordID_t CCRCEngine<false>::DoCrc ( const BYTE * pWord, int iLen )
{
	return (SphWordID_t) sphFNV64 ( pWord, iLen );
}


template < bool CRC32DICT >
SphWordID_t CSphDictCRC<CRC32DICT>::GetWordID ( BYTE * pWord )
{
	// apply stopword filter before stemmers
	if ( GetSettings().m_bStopwordsUnstemmed && !FilterStopword ( tHASH::DoCrc ( pWord ) ) )
		return 0;

	// skip stemmers for magic words
	if ( pWord[0]>=0x20 )
		ApplyStemmers ( pWord );

	// stemmer might squeeze out the word
	if ( !pWord[0] )
		return 0;

	return GetSettings().m_bStopwordsUnstemmed
		? tHASH::DoCrc ( pWord )
		: FilterStopword ( tHASH::DoCrc ( pWord ) );
}


template < bool CRC32DICT >
SphWordID_t CSphDictCRC<CRC32DICT>::GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops )
{
	SphWordID_t uId = tHASH::DoCrc ( pWord, iLen );
	return bFilterStops ? FilterStopword ( uId ) : uId;
}


template < bool CRC32DICT >
SphWordID_t CSphDictCRC<CRC32DICT>::GetWordIDWithMarkers ( BYTE * pWord )
{
	ApplyStemmers ( pWord + 1 );
	SphWordID_t uWordId = tHASH::DoCrc ( pWord + 1 );
	int iLength = strlen ( (const char *)(pWord + 1) );
	pWord [iLength + 1] = MAGIC_WORD_TAIL;
	pWord [iLength + 2] = '\0';
	return FilterStopword ( uWordId ) ? tHASH::DoCrc ( pWord ) : 0;
}


template < bool CRC32DICT >
SphWordID_t CSphDictCRC<CRC32DICT>::GetWordIDNonStemmed ( BYTE * pWord )
{
	SphWordID_t uWordId = tHASH::DoCrc ( pWord + 1 );
	if ( !FilterStopword ( uWordId ) )
		return 0;

	return tHASH::DoCrc ( pWord );
}


template < bool CRC32DICT >
bool CSphDictCRC<CRC32DICT>::IsStopWord ( const BYTE * pWord ) const
{
	return FilterStopword ( tHASH::DoCrc ( pWord ) )==0;
}


//////////////////////////////////////////////////////////////////////////
SphWordID_t CSphDictTemplate::GetWordID ( BYTE * pWord )
{
	// apply stopword filter before stemmers
	if ( GetSettings().m_bStopwordsUnstemmed && !FilterStopword ( DoCrc ( pWord ) ) )
		return 0;

	// skip stemmers for magic words
	if ( pWord[0]>=0x20 )
		ApplyStemmers ( pWord );

	return GetSettings().m_bStopwordsUnstemmed
		? DoCrc ( pWord )
		: FilterStopword ( DoCrc ( pWord ) );
}


SphWordID_t CSphDictTemplate::GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops )
{
	SphWordID_t uId = DoCrc ( pWord, iLen );
	return bFilterStops ? FilterStopword ( uId ) : uId;
}


SphWordID_t CSphDictTemplate::GetWordIDWithMarkers ( BYTE * pWord )
{
	ApplyStemmers ( pWord + 1 );
	// stemmer might squeeze out the word
	if ( !pWord[1] )
		return 0;
	SphWordID_t uWordId = DoCrc ( pWord + 1 );
	int iLength = strlen ( (const char *)(pWord + 1) );
	pWord [iLength + 1] = MAGIC_WORD_TAIL;
	pWord [iLength + 2] = '\0';
	return FilterStopword ( uWordId ) ? DoCrc ( pWord ) : 0;
}


SphWordID_t CSphDictTemplate::GetWordIDNonStemmed ( BYTE * pWord )
{
	SphWordID_t uWordId = DoCrc ( pWord + 1 );
	if ( !FilterStopword ( uWordId ) )
		return 0;

	return DoCrc ( pWord );
}

bool CSphDictTemplate::IsStopWord ( const BYTE * pWord ) const
{
	return FilterStopword ( DoCrc ( pWord ) )==0;
}

//////////////////////////////////////////////////////////////////////////

void CSphTemplateDictTraits::LoadStopwords ( const char * sFiles, const ISphTokenizer * pTokenizer )
{
	assert ( !m_pStopwords );
	assert ( !m_iStopwords );

	// tokenize file list
	if ( !sFiles || !*sFiles )
		return;

	m_dSWFileInfos.Resize ( 0 );

	CSphScopedPtr<ISphTokenizer> tTokenizer ( pTokenizer->Clone ( SPH_CLONE_INDEX ) );
	CSphFixedVector<char> dList ( 1+strlen(sFiles) );
	strcpy ( dList.Begin(), sFiles ); // NOLINT

	char * pCur = dList.Begin();
	char * sName = NULL;

	CSphVector<SphWordID_t> dStop;

	for ( ;; )
	{
		// find next name start
		while ( *pCur && isspace(*pCur) ) pCur++;
		if ( !*pCur ) break;
		sName = pCur;

		// find next name end
		while ( *pCur && !isspace(*pCur) ) pCur++;
		if ( *pCur ) *pCur++ = '\0';

		BYTE * pBuffer = NULL;

		CSphSavedFile tInfo;
		tInfo.m_sFilename = sName;
		GetFileStats ( sName, tInfo, NULL );
		m_dSWFileInfos.Add ( tInfo );

		// open file
		struct_stat st;
		if ( stat ( sName, &st )==0 )
			pBuffer = new BYTE [(size_t)st.st_size];
		else
		{
			sphWarn ( "stopwords: failed to get file size for '%s'", sName );
			continue;
		}

		FILE * fp = fopen ( sName, "rb" );
		if ( !fp )
		{
			sphWarn ( "failed to load stopwords from '%s'", sName );
			SafeDeleteArray ( pBuffer );
			continue;
		}

		// tokenize file
		int iLength = (int)fread ( pBuffer, 1, (size_t)st.st_size, fp );

		BYTE * pToken;
		tTokenizer->SetBuffer ( pBuffer, iLength );
		while ( ( pToken = tTokenizer->GetToken() )!=NULL )
			if ( m_tSettings.m_bStopwordsUnstemmed )
				dStop.Add ( GetWordIDNonStemmed ( pToken ) );
			else
				dStop.Add ( GetWordID ( pToken ) );

		// close file
		fclose ( fp );

		SafeDeleteArray ( pBuffer );
	}

	// sort stopwords
	dStop.Uniq();

	// store IDs
	if ( dStop.GetLength() )
	{
		m_dStopwordContainer.Reset ( dStop.GetLength() );
		ARRAY_FOREACH ( i, dStop )
			m_dStopwordContainer[i] = dStop[i];

		m_iStopwords = m_dStopwordContainer.GetLength ();
		m_pStopwords = m_dStopwordContainer.Begin();
	}
}


void CSphTemplateDictTraits::LoadStopwords ( const CSphVector<SphWordID_t> & dStopwords )
{
	m_dStopwordContainer.Reset ( dStopwords.GetLength() );
	ARRAY_FOREACH ( i, dStopwords )
		m_dStopwordContainer[i] = dStopwords[i];

	m_iStopwords = m_dStopwordContainer.GetLength ();
	m_pStopwords = m_dStopwordContainer.Begin();
}


void CSphTemplateDictTraits::WriteStopwords ( CSphWriter & tWriter )
{
	tWriter.PutDword ( (DWORD)m_iStopwords );
	for ( int i = 0; i < m_iStopwords; i++ )
		tWriter.ZipOffset ( m_pStopwords[i] );
}


void CSphTemplateDictTraits::SweepWordformContainers ( const CSphVector<CSphSavedFile> & dFiles )
{
	for ( int i = 0; i < m_dWordformContainers.GetLength (); )
	{
		CSphWordforms * WC = m_dWordformContainers[i];
		if ( WC->m_iRefCount==0 && !WC->IsEqual ( dFiles ) )
		{
			delete WC;
			m_dWordformContainers.Remove ( i );
		} else
			++i;
	}
}


static const char * ConcatReportStrings ( const CSphVector<CSphString> & dStrings )
{
	const int MAX_REPORT_LEN = 1024;
	static char szReport[MAX_REPORT_LEN];
	szReport[0] = '\0';

	ARRAY_FOREACH ( i, dStrings )
	{
		int iLen = strlen ( szReport );
		if ( iLen + dStrings[i].Length() + 2 > MAX_REPORT_LEN )
			break;

		strcat ( szReport, dStrings[i].cstr() );	// NOLINT
		iLen += dStrings[i].Length();
		if ( i < dStrings.GetLength()-1 )
		{
			szReport[iLen] = ' ';
			szReport[iLen+1] = '\0';
		} else
			szReport[iLen] = '\0';
	}

	return szReport;
}


CSphWordforms * CSphTemplateDictTraits::GetWordformContainer ( const CSphVector<CSphSavedFile> & dFileInfos,
	const CSphVector<CSphString> * pEmbedded, const ISphTokenizer * pTokenizer, const char * sIndex )
{
	uint64_t uTokenizerFNV = pTokenizer->GetSettingsFNV();
	ARRAY_FOREACH ( i, m_dWordformContainers )
		if ( m_dWordformContainers[i]->IsEqual ( dFileInfos ) )
		{
			CSphWordforms * pContainer = m_dWordformContainers[i];
			if ( uTokenizerFNV==pContainer->m_uTokenizerFNV )
				return pContainer;

			CSphVector<CSphString> dErrorReport;
			ARRAY_FOREACH ( j, dFileInfos )
				dErrorReport.Add ( dFileInfos[j].m_sFilename );

			const char * szAllFiles = ConcatReportStrings ( dErrorReport );
			sphWarning ( "index '%s': wordforms file '%s' is shared with index '%s', "
				"but tokenizer settings are different",
				sIndex, szAllFiles, pContainer->m_sIndexName.cstr() );
		}

	CSphWordforms * pContainer = LoadWordformContainer ( dFileInfos, pEmbedded, pTokenizer, sIndex );
	if ( pContainer )
		m_dWordformContainers.Add ( pContainer );

	return pContainer;
}


void CSphTemplateDictTraits::AddWordform ( CSphWordforms * pContainer, char * sBuffer, int iLen,
	ISphTokenizer * pTokenizer, const char * szFile, const CSphVector<char> & dBlended )
{
	static CSphVector<CSphString> dTokens;
	bool bSeparatorFound = false;
	bool bAfterMorphology = false;

	// parse the line
	pTokenizer->SetBuffer ( (BYTE*)sBuffer, iLen );

	bool bFirstToken = true;
	bool bStopwordsPresent = false;
	dTokens.Resize ( 0 );

	BYTE * pFrom = NULL;
	while ( ( pFrom = pTokenizer->GetToken () )!=NULL )
	{
		if ( *pFrom=='#' )
			break;

		if ( *pFrom=='~' && bFirstToken )
		{
			bAfterMorphology = true;
			bFirstToken = false;
			continue;
		}

		bFirstToken = false;

		if ( *pFrom=='>' )
		{
			bSeparatorFound = true;
			break;
		}

		if ( *pFrom=='=' && *pTokenizer->GetBufferPtr()=='>' )
		{
			pTokenizer->GetToken();
			bSeparatorFound = true;
			break;
		}

		if ( GetWordID ( pFrom, strlen ( (const char*)pFrom ), true ) )
			dTokens.Add ( (const char*)pFrom );
		else
			bStopwordsPresent = true;
	}

	if ( !dTokens.GetLength() )
	{
		sphWarning ( "index '%s': all source tokens are stopwords (wordform='%s', file='%s'). IGNORED.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );
		return;
	}

	if ( !bSeparatorFound )
	{
		sphWarning ( "index '%s': no wordform separator found (wordform='%s', file='%s'). IGNORED.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );
		return;
	}

	BYTE * pTo = pTokenizer->GetToken ();
	int iToTokenLenCP = pTokenizer->GetLastTokenLen();
	if ( !pTo )
	{
		sphWarning ( "index '%s': no destination token found (wordform='%s', file='%s'). IGNORED.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );
		return;
	}

	if ( *pTo=='#' )
	{
		sphWarning ( "index '%s': misplaced comment (wordform='%s', file='%s'). IGNORED.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );
		return;
	}

	CSphString sTo ( (const char *)pTo );
	if ( !GetWordID ( pTo, sTo.Length(), true ) )
	{
		sphWarning ( "index '%s': destination token is a stopwords (wordform='%s', file='%s'). IGNORED.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );
		return;
	}

	// what if we have more than one word in the right part?
	bool bMultipleDests = false;
	while ( pTokenizer->GetToken() )
		bMultipleDests = true;

	if ( bMultipleDests )
	{
		sphWarning ( "index '%s': invalid mapping (must be exactly 1 destination keyword) (wordform='%s', file='%s'). IGNORED.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );
		return;
	}

	if ( bStopwordsPresent )
		sphWarning ( "index '%s': wordform contains stopwords (wordform='%s'). Fix your wordforms file '%s'.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );

	// we disabled all blended, so we need to filter them manually
	bool bBlendedPresent = false;
	for ( int i = 0; i < sTo.Length() && !bBlendedPresent; i++ )
		ARRAY_FOREACH_COND ( j, dBlended, !bBlendedPresent )
			bBlendedPresent = sTo.cstr()[i]==dBlended[j];

	if ( bBlendedPresent )
		sphWarning ( "invalid mapping (destination contains blended characters) (wordform='%s'). Fix your wordforms file '%s'.", sBuffer, szFile );

	if ( dTokens.GetLength()>1 )
	{
		CSphMultiform * pMultiWordform = new CSphMultiform;
		pMultiWordform->m_sNormalForm = sTo;
		pMultiWordform->m_iNormalTokenLen = iToTokenLenCP;

		for ( int i = 1; i < dTokens.GetLength(); i++ )
			pMultiWordform->m_dTokens.Add ( dTokens[i] );

		if ( !pContainer->m_pMultiWordforms )
			pContainer->m_pMultiWordforms = new CSphMultiformContainer;

		CSphMultiforms ** pWordforms = pContainer->m_pMultiWordforms->m_Hash ( dTokens[0] );
		if ( pWordforms )
		{
			ARRAY_FOREACH ( iMultiform, (*pWordforms)->m_pForms )
			{
				CSphMultiform * pStoredMF = (*pWordforms)->m_pForms[iMultiform];
				if ( pStoredMF->m_dTokens.GetLength()==pMultiWordform->m_dTokens.GetLength() )
				{
					bool bSameTokens = true;
					ARRAY_FOREACH_COND ( iToken, pStoredMF->m_dTokens, bSameTokens )
						if ( pStoredMF->m_dTokens[iToken]!=pMultiWordform->m_dTokens[iToken] )
							bSameTokens = false;

					if ( bSameTokens )
					{
						const char * szStoredTokens = ConcatReportStrings ( pStoredMF->m_dTokens );
						sphWarning ( "index '%s': duplicate wordform found - overridden ( current='%s', old='%s %s > %s' ). Fix your wordforms file '%s'.",
							pContainer->m_sIndexName.cstr(), sBuffer, dTokens[0].cstr(), szStoredTokens, pStoredMF->m_sNormalForm.cstr(), szFile );

						pStoredMF->m_iNormalTokenLen = pMultiWordform->m_iNormalTokenLen;
						pStoredMF->m_sNormalForm = pMultiWordform->m_sNormalForm;
						SafeDelete ( pMultiWordform );
						break; // otherwise, we crash next turn
					}
				}
			}

			if ( pMultiWordform )
			{
				(*pWordforms)->m_pForms.Add ( pMultiWordform );
				(*pWordforms)->m_iMinTokens = Min ( (*pWordforms)->m_iMinTokens, pMultiWordform->m_dTokens.GetLength () );
				(*pWordforms)->m_iMaxTokens = Max ( (*pWordforms)->m_iMaxTokens, pMultiWordform->m_dTokens.GetLength () );
				pContainer->m_pMultiWordforms->m_iMaxTokens = Max ( pContainer->m_pMultiWordforms->m_iMaxTokens, (*pWordforms)->m_iMaxTokens );
			}
		} else
		{
			CSphMultiforms * pNewWordforms = new CSphMultiforms;
			pNewWordforms->m_pForms.Add ( pMultiWordform );
			pNewWordforms->m_iMinTokens = pMultiWordform->m_dTokens.GetLength ();
			pNewWordforms->m_iMaxTokens = pMultiWordform->m_dTokens.GetLength ();
			pContainer->m_pMultiWordforms->m_iMaxTokens = Max ( pContainer->m_pMultiWordforms->m_iMaxTokens, pNewWordforms->m_iMaxTokens );
			pContainer->m_pMultiWordforms->m_Hash.Add ( pNewWordforms, dTokens[0] );
		}
	} else
	{
		if ( bAfterMorphology )
		{
			BYTE pBuf [16+3*SPH_MAX_WORD_LEN];
			memcpy ( pBuf, dTokens[0].cstr(), dTokens[0].Length()+1 );
			ApplyStemmers ( pBuf );
			dTokens[0] = (char *)pBuf;
		}

		// check wordform that source token is a new token or has same destination token
		int * pRefTo = pContainer->m_dHash ( dTokens[0] );
		assert ( !pRefTo || ( *pRefTo>=0 && *pRefTo<pContainer->m_dNormalForms.GetLength() ) );
		if ( pRefTo )
		{
			// replace with a new wordform
			if ( pContainer->m_dNormalForms[*pRefTo].m_sWord!=sTo || pContainer->m_dNormalForms[*pRefTo].m_bAfterMorphology!=bAfterMorphology )
			{
				CSphStoredNF & tRefTo = pContainer->m_dNormalForms[*pRefTo];
				sphWarning ( "index '%s': duplicate wordform found - overridden ( current='%s', old='%s%s > %s' ). Fix your wordforms file '%s'.",
					pContainer->m_sIndexName.cstr(), sBuffer, tRefTo.m_bAfterMorphology ? "~" : "", dTokens[0].cstr(), tRefTo.m_sWord.cstr(), szFile );

				tRefTo.m_sWord = sTo;
				tRefTo.m_bAfterMorphology = bAfterMorphology;
				pContainer->m_bHavePostMorphNF |= bAfterMorphology;
			} else
				sphWarning ( "index '%s': duplicate wordform found ( '%s' ). Fix your wordforms file '%s'.", pContainer->m_sIndexName.cstr(), sBuffer, szFile );
		} else
		{
			CSphStoredNF tForm;
			tForm.m_sWord = sTo;
			tForm.m_bAfterMorphology = bAfterMorphology;
			pContainer->m_bHavePostMorphNF |= bAfterMorphology;
			if ( !pContainer->m_dNormalForms.GetLength()
				|| pContainer->m_dNormalForms.Last().m_sWord!=sTo
				|| pContainer->m_dNormalForms.Last().m_bAfterMorphology!=bAfterMorphology)
				pContainer->m_dNormalForms.Add ( tForm );

			pContainer->m_dHash.Add ( pContainer->m_dNormalForms.GetLength()-1, dTokens[0] );
		}
	}
}


CSphWordforms * CSphTemplateDictTraits::LoadWordformContainer ( const CSphVector<CSphSavedFile> & dFileInfos,
	const CSphVector<CSphString> * pEmbeddedWordforms, const ISphTokenizer * pTokenizer, const char * sIndex )
{
	// allocate it
	CSphWordforms * pContainer = new CSphWordforms();
	pContainer->m_dFiles = dFileInfos;
	pContainer->m_uTokenizerFNV = pTokenizer->GetSettingsFNV();
	pContainer->m_sIndexName = sIndex;

	CSphScopedPtr<ISphTokenizer> pMyTokenizer ( pTokenizer->Clone ( SPH_CLONE_INDEX ) );
	const CSphTokenizerSettings & tSettings = pMyTokenizer->GetSettings();
	CSphVector<char> dBlended;

	// get a list of blend chars and set add them to the tokenizer as simple chars
	if ( tSettings.m_sBlendChars.Length() )
	{
		CSphVector<char> dNewCharset;
		dNewCharset.Resize ( tSettings.m_sCaseFolding.Length() );
		memcpy ( dNewCharset.Begin(), tSettings.m_sCaseFolding.cstr(), dNewCharset.GetLength() );

		CSphVector<CSphRemapRange> dRemaps;
		CSphCharsetDefinitionParser tParser;
		if ( tParser.Parse ( tSettings.m_sBlendChars.cstr(), dRemaps ) )
			ARRAY_FOREACH ( i, dRemaps )
				for ( int j = dRemaps[i].m_iStart; j<=dRemaps[i].m_iEnd; j++ )
				{
					dNewCharset.Add ( ',' );
					dNewCharset.Add ( ' ' );
					dNewCharset.Add ( char(j) );
					dBlended.Add ( char(j) );
				}

		dNewCharset.Add(0);

		CSphString sError;
		pMyTokenizer->SetCaseFolding ( dNewCharset.Begin(), sError );

		// disable blend chars
		pMyTokenizer->SetBlendChars ( NULL, sError );
	}

	// add wordform-specific specials
	pMyTokenizer->AddSpecials ( "#=>~" );

	if ( pEmbeddedWordforms )
	{
		CSphVector<CSphString> dFilenames;
		dFilenames.Resize ( dFileInfos.GetLength() );
		ARRAY_FOREACH ( i, dFileInfos )
			dFilenames[i] = dFileInfos[i].m_sFilename;

		CSphString sAllFiles = ConcatReportStrings ( dFilenames );

		ARRAY_FOREACH ( i, (*pEmbeddedWordforms) )
			AddWordform ( pContainer, (char*)(*pEmbeddedWordforms)[i].cstr(),
				(*pEmbeddedWordforms)[i].Length(), pMyTokenizer.Ptr(), sAllFiles.cstr(), dBlended );
	} else
	{
		char sBuffer [ 6*SPH_MAX_WORD_LEN + 512 ]; // enough to hold 2 UTF-8 words, plus some whitespace overhead

		ARRAY_FOREACH ( i, dFileInfos )
		{
			CSphAutoreader rdWordforms;
			const char * szFile = dFileInfos[i].m_sFilename.cstr();
			CSphString sError;
			if ( !rdWordforms.Open ( szFile, sError ) )
			{
				sphWarning ( "index '%s': %s", sIndex, sError.cstr() );
				return NULL;
			}

			int iLen;
			while ( ( iLen = rdWordforms.GetLine ( sBuffer, sizeof(sBuffer) ) )>=0 )
				AddWordform ( pContainer, sBuffer, iLen, pMyTokenizer.Ptr(), szFile, dBlended );
		}
	}

	return pContainer;
}


bool CSphTemplateDictTraits::LoadWordforms ( const CSphVector<CSphString> & dFiles,
	const CSphEmbeddedFiles * pEmbedded, const ISphTokenizer * pTokenizer, const char * sIndex )
{
	if ( pEmbedded )
	{
		m_dWFFileInfos.Resize ( pEmbedded->m_dWordformFiles.GetLength() );
		ARRAY_FOREACH ( i, m_dWFFileInfos )
			m_dWFFileInfos[i] = pEmbedded->m_dWordformFiles[i];
	} else
	{
		m_dWFFileInfos.Reserve ( dFiles.GetLength() );
		CSphSavedFile tFile;
		ARRAY_FOREACH ( i, dFiles )
			if ( !dFiles[i].IsEmpty() )
			{
				if ( GetFileStats ( dFiles[i].cstr(), tFile, NULL ) )
					m_dWFFileInfos.Add ( tFile );
				else
					sphWarning ( "index '%s': wordforms file '%s' not found", sIndex, dFiles[i].cstr() );
			}
	}

	if ( !m_dWFFileInfos.GetLength() )
		return false;

	SweepWordformContainers ( m_dWFFileInfos );

	m_pWordforms = GetWordformContainer ( m_dWFFileInfos, pEmbedded ? &(pEmbedded->m_dWordforms) : NULL, pTokenizer, sIndex );
	if ( m_pWordforms )
	{
		m_pWordforms->m_iRefCount++;
		if ( m_pWordforms->m_bHavePostMorphNF && !m_dMorph.GetLength() )
			sphWarning ( "index '%s': wordforms contain post-morphology normal forms, but no morphology was specified", sIndex );
	}

	return !!m_pWordforms;
}


void CSphTemplateDictTraits::WriteWordforms ( CSphWriter & tWriter )
{
	if ( !m_pWordforms )
	{
		tWriter.PutDword(0);
		return;
	}

	int nMultiforms = 0;
	if ( m_pWordforms->m_pMultiWordforms )
	{
		CSphMultiformContainer::CSphMultiformHash & tHash = m_pWordforms->m_pMultiWordforms->m_Hash;
		tHash.IterateStart();
		while ( tHash.IterateNext() )
		{
			CSphMultiforms * pMF = tHash.IterateGet();
			nMultiforms += pMF ? pMF->m_pForms.GetLength() : 0;
		}
	}

	tWriter.PutDword ( m_pWordforms->m_dHash.GetLength()+nMultiforms );
	m_pWordforms->m_dHash.IterateStart();
	while ( m_pWordforms->m_dHash.IterateNext() )
	{
		const CSphString & sKey = m_pWordforms->m_dHash.IterateGetKey();
		int iIndex = m_pWordforms->m_dHash.IterateGet();
		CSphString sLine;
		sLine.SetSprintf ( "%s%s > %s", m_pWordforms->m_dNormalForms[iIndex].m_bAfterMorphology ? "~" : "",
			sKey.cstr(), m_pWordforms->m_dNormalForms[iIndex].m_sWord.cstr() );
		tWriter.PutString ( sLine );
	}

	if ( m_pWordforms->m_pMultiWordforms )
	{
		CSphMultiformContainer::CSphMultiformHash & tHash = m_pWordforms->m_pMultiWordforms->m_Hash;
		tHash.IterateStart();
		while ( tHash.IterateNext() )
		{
			const CSphString & sKey = tHash.IterateGetKey();
			CSphMultiforms * pMF = tHash.IterateGet();
			if ( !pMF )
				continue;

			ARRAY_FOREACH ( i, pMF->m_pForms )
			{
				CSphString sLine;
				const char * szTokens = ConcatReportStrings ( pMF->m_pForms[i]->m_dTokens );
				sLine.SetSprintf ( "%s %s > %s", sKey.cstr(), szTokens, pMF->m_pForms[i]->m_sNormalForm.cstr() );
				tWriter.PutString ( sLine );
			}
		}
	}
}


int CSphTemplateDictTraits::SetMorphology ( const char * szMorph, bool bUseUTF8, CSphString & sMessage )
{
	m_dMorph.Reset ();
#if USE_LIBSTEMMER
	ARRAY_FOREACH ( i, m_dStemmers )
		sb_stemmer_delete ( m_dStemmers[i] );
	m_dStemmers.Reset ();
#endif

	if ( !szMorph )
		return ST_OK;

	CSphString sOption = szMorph;
	sOption.ToLower ();

	CSphString sError;
	int iRes = ParseMorphology ( sOption.cstr(), bUseUTF8, sMessage );
	if ( iRes==ST_WARNING && sMessage.IsEmpty() )
		sMessage.SetSprintf ( "invalid morphology option %s; skipped", sOption.cstr() );
	return iRes;
}


bool CSphTemplateDictTraits::HasMorphology() const
{
	return ( m_dMorph.GetLength()>0 );
}


/// common id-based stemmer
bool CSphTemplateDictTraits::StemById ( BYTE * pWord, int iStemmer )
{
	char szBuf [ MAX_KEYWORD_BYTES ];

	// safe quick strncpy without (!) padding and with a side of strlen
	char * p = szBuf;
	char * pMax = szBuf + sizeof(szBuf) - 1;
	BYTE * pLastSBS = NULL;
	while ( *pWord && p<pMax )
	{
		pLastSBS = ( *pWord )<0x80 ? pWord : pLastSBS;
		*p++ = *pWord++;
	}
	int iLen = p - szBuf;
	*p = '\0';
	pWord -= iLen;

	switch ( iStemmer )
	{
	case SPH_MORPH_STEM_EN:
		stem_en ( pWord, iLen );
		break;

	case SPH_MORPH_STEM_RU_CP1251:
		stem_ru_cp1251 ( pWord );
		break;

	case SPH_MORPH_STEM_RU_UTF8:
		// skip stemming in case of SBC at the end of the word
		if ( pLastSBS && ( pLastSBS-pWord+1 )>=iLen )
			break;

		// stem only UTF8 tail
		if ( !pLastSBS )
		{
			stem_ru_utf8 ( (WORD*)pWord );
		} else
		{
			stem_ru_utf8 ( (WORD *)( pLastSBS+1 ) );
		}
		break;

	case SPH_MORPH_STEM_CZ:
		stem_cz ( pWord );
		break;

	case SPH_MORPH_STEM_AR_UTF8:
		stem_ar_utf8 ( pWord );
		break;

	case SPH_MORPH_SOUNDEX:
		stem_soundex ( pWord );
		break;

	case SPH_MORPH_METAPHONE_SBCS:
		stem_dmetaphone ( pWord, false );
		break;

	case SPH_MORPH_METAPHONE_UTF8:
		stem_dmetaphone ( pWord, true );
		break;

	case SPH_MORPH_AOTLEMMER_RU_CP1251:
		sphAotLemmatizeRu1251 ( pWord );
		break;

	case SPH_MORPH_AOTLEMMER_RU_UTF8:
		sphAotLemmatizeRuUTF8 ( pWord );
		break;

	case SPH_MORPH_AOTLEMMER_EN:
		sphAotLemmatize ( pWord, AOT_EN );
		break;

	case SPH_MORPH_AOTLEMMER_DE_UTF8:
		sphAotLemmatizeDeUTF8 ( pWord );
		break;

	case SPH_MORPH_AOTLEMMER_DE_CP1252:
		sphAotLemmatizeDe1252 ( pWord );
		break;

	case SPH_MORPH_AOTLEMMER_RU_ALL:
	case SPH_MORPH_AOTLEMMER_EN_ALL:
	case SPH_MORPH_AOTLEMMER_DE_ALL:
		// do the real work somewhere else
		// this is mostly for warning suppressing and making some features like
		// index_exact_words=1 vs expand_keywords=1 work
		break;

	default:
#if USE_LIBSTEMMER
		if ( iStemmer>=SPH_MORPH_LIBSTEMMER_FIRST && iStemmer<SPH_MORPH_LIBSTEMMER_LAST )
		{
			sb_stemmer * pStemmer = m_dStemmers [iStemmer - SPH_MORPH_LIBSTEMMER_FIRST];
			assert ( pStemmer );

			const sb_symbol * sStemmed = sb_stemmer_stem ( pStemmer, (sb_symbol*)pWord, strlen ( (const char*)pWord ) );
			int iLen = sb_stemmer_length ( pStemmer );

			memcpy ( pWord, sStemmed, iLen );
			pWord[iLen] = '\0';
		} else
			return false;

	break;
#else
		return false;
#endif
	}

	return strcmp ( (char *)pWord, szBuf )!=0;
}

void CSphDiskDictTraits::DictBegin ( CSphAutofile & , CSphAutofile & tDict, int, ThrottleState_t * pThrottle )
{
	m_wrDict.CloseFile ();
	m_wrDict.SetFile ( tDict, NULL, m_sWriterError );
	m_wrDict.SetThrottle ( pThrottle );
	m_wrDict.PutByte ( 1 );
}

bool CSphDiskDictTraits::DictEnd ( DictHeader_t * pHeader, int, CSphString & sError, ThrottleState_t * )
{
	// flush wordlist checkpoints
	pHeader->m_iDictCheckpointsOffset = m_wrDict.GetPos();
	pHeader->m_iDictCheckpoints = m_dCheckpoints.GetLength();
	ARRAY_FOREACH ( i, m_dCheckpoints )
	{
		assert ( m_dCheckpoints[i].m_iWordlistOffset );
		m_wrDict.PutOffset ( m_dCheckpoints[i].m_iWordID );
		m_wrDict.PutOffset ( m_dCheckpoints[i].m_iWordlistOffset );
	}

	// done
	m_wrDict.CloseFile ();
	if ( m_wrDict.IsError() )
		sError = m_sWriterError;
	return !m_wrDict.IsError();
}

void CSphDiskDictTraits::DictEntry ( const CSphDictEntry & tEntry )
{
	// insert wordlist checkpoint
	if ( ( m_iEntries % SPH_WORDLIST_CHECKPOINT )==0 )
	{
		if ( m_iEntries ) // but not the 1st entry
		{
			assert ( tEntry.m_iDoclistOffset > m_iLastDoclistPos );
			m_wrDict.ZipInt ( 0 ); // indicate checkpoint
			m_wrDict.ZipOffset ( tEntry.m_iDoclistOffset - m_iLastDoclistPos ); // store last length
		}

		// restart delta coding, once per SPH_WORDLIST_CHECKPOINT entries
		m_iLastWordID = 0;
		m_iLastDoclistPos = 0;

		// begin new wordlist entry
		assert ( m_wrDict.GetPos()<=UINT_MAX );

		CSphWordlistCheckpoint & tCheckpoint = m_dCheckpoints.Add();
		tCheckpoint.m_iWordID = tEntry.m_uWordID;
		tCheckpoint.m_iWordlistOffset = m_wrDict.GetPos();
	}

	assert ( tEntry.m_iDoclistOffset>m_iLastDoclistPos );
	m_wrDict.ZipOffset ( tEntry.m_uWordID - m_iLastWordID ); // FIXME! slow with 32bit wordids
	m_wrDict.ZipOffset ( tEntry.m_iDoclistOffset - m_iLastDoclistPos );

	m_iLastWordID = tEntry.m_uWordID;
	m_iLastDoclistPos = tEntry.m_iDoclistOffset;

	assert ( tEntry.m_iDocs );
	assert ( tEntry.m_iHits );
	m_wrDict.ZipInt ( tEntry.m_iDocs );
	m_wrDict.ZipInt ( tEntry.m_iHits );

	// write skiplist location info, if any
	if ( tEntry.m_iDocs > SPH_SKIPLIST_BLOCK )
		m_wrDict.ZipOffset ( tEntry.m_iSkiplistOffset );

	m_iEntries++;
}

void CSphDiskDictTraits::DictEndEntries ( SphOffset_t iDoclistOffset )
{
	assert ( iDoclistOffset>=m_iLastDoclistPos );
	m_wrDict.ZipInt ( 0 ); // indicate checkpoint
	m_wrDict.ZipOffset ( iDoclistOffset - m_iLastDoclistPos ); // store last doclist length
}

//////////////////////////////////////////////////////////////////////////
// KEYWORDS STORING DICTIONARY, INFIX HASH BUILDER
//////////////////////////////////////////////////////////////////////////

template < int SIZE >
struct Infix_t
{
	DWORD m_Data[SIZE];

#ifndef NDEBUG
	BYTE m_TrailingZero;

	Infix_t ()
		: m_TrailingZero ( 0 )
	{}
#endif

	void Reset ()
	{
		for ( int i=0; i<SIZE; i++ )
			m_Data[i] = 0;
	}

	bool operator == ( const Infix_t<SIZE> & rhs ) const;
};


template<>
bool Infix_t<2>::operator == ( const Infix_t<2> & rhs ) const
{
	return m_Data[0]==rhs.m_Data[0] && m_Data[1]==rhs.m_Data[1];
};


template<>
bool Infix_t<3>::operator == ( const Infix_t<3> & rhs ) const
{
	return m_Data[0]==rhs.m_Data[0] && m_Data[1]==rhs.m_Data[1] && m_Data[2]==rhs.m_Data[2];
};


template<>
bool Infix_t<5>::operator == ( const Infix_t<5> & rhs ) const
{
	return m_Data[0]==rhs.m_Data[0] && m_Data[1]==rhs.m_Data[1] && m_Data[2]==rhs.m_Data[2]
		&& m_Data[3]==rhs.m_Data[3] && m_Data[4]==rhs.m_Data[4];
};


struct InfixIntvec_t
{
public:
	// do not change the order of fields in this union - it matters a lot
	union
	{
		DWORD			m_dData[4];
		struct
		{
			int				m_iDynLen;
			int				m_iDynLimit;
			DWORD *			m_pDynData;
		};
	};

public:
	InfixIntvec_t()
	{
		m_dData[0] = 0;
		m_dData[1] = 0;
		m_dData[2] = 0;
		m_dData[3] = 0;
	}

	~InfixIntvec_t()
	{
		if ( IsDynamic() )
			SafeDeleteArray ( m_pDynData );
	}

	bool IsDynamic() const
	{
		return ( m_dData[0] & 0x80000000UL )!=0;
	}

	void Add ( DWORD uVal )
	{
		if ( !m_dData[0] )
		{
			// empty
			m_dData[0] = uVal | ( 1UL<<24 );

		} else if ( !IsDynamic() )
		{
			// 1..4 static entries
			int iLen = m_dData[0] >> 24;
			DWORD uLast = m_dData [ iLen-1 ] & 0xffffffUL;

			// redundant
			if ( uVal==uLast )
				return;

			// grow static part
			if ( iLen<4 )
			{
				m_dData[iLen] = uVal;
				m_dData[0] = ( m_dData[0] & 0xffffffUL ) | ( ++iLen<<24 );
				return;
			}

			// dynamize
			DWORD * pDyn = new DWORD[16];
			pDyn[0] = m_dData[0] & 0xffffffUL;
			pDyn[1] = m_dData[1];
			pDyn[2] = m_dData[2];
			pDyn[3] = m_dData[3];
			pDyn[4] = uVal;
			m_iDynLen = 0x80000005UL; // dynamic flag, len=5
			m_iDynLimit = 16; // limit=16
			m_pDynData = pDyn;

		} else
		{
			// N dynamic entries
			int iLen = m_iDynLen & 0xffffffUL;
			if ( uVal==m_pDynData[iLen-1] )
				return;
			if ( iLen>=m_iDynLimit )
			{
				m_iDynLimit *= 2;
				DWORD * pNew = new DWORD [ m_iDynLimit ];
				for ( int i=0; i<iLen; i++ )
					pNew[i] = m_pDynData[i];
				SafeDeleteArray ( m_pDynData );
				m_pDynData = pNew;
			}

			m_pDynData[iLen] = uVal;
			m_iDynLen++;
		}
	}

	bool operator == ( const InfixIntvec_t & rhs ) const
	{
		// check dynflag, length, maybe first element
		if ( m_dData[0]!=rhs.m_dData[0] )
			return false;

		// check static data
		if ( !IsDynamic() )
		{
			for ( int i=1; i<(int)(m_dData[0]>>24); i++ )
				if ( m_dData[i]!=rhs.m_dData[i] )
					return false;
			return true;
		}

		// check dynamic data
		const DWORD * a = m_pDynData;
		const DWORD * b = rhs.m_pDynData;
		const DWORD * m = a + ( m_iDynLen & 0xffffffUL );
		while ( a<m )
			if ( *a++!=*b++ )
				return false;
		return true;
	}

public:
	int GetLength() const
	{
		if ( !IsDynamic() )
			return m_dData[0] >> 24;
		return m_iDynLen & 0xffffffUL;
	}

	DWORD operator[] ( int iIndex )const
	{
		if ( !IsDynamic() )
			return m_dData[iIndex] & 0xffffffUL;
		return m_pDynData[iIndex];
	}
};


void Swap ( InfixIntvec_t & a, InfixIntvec_t & b )
{
	::Swap ( a.m_dData[0], b.m_dData[0] );
	::Swap ( a.m_dData[1], b.m_dData[1] );
	::Swap ( a.m_dData[2], b.m_dData[2] );
	::Swap ( a.m_dData[3], b.m_dData[3] );
}


template < int SIZE >
struct InfixHashEntry_t
{
	Infix_t<SIZE>	m_tKey;		///< key, owned by the hash
	InfixIntvec_t	m_tValue;	///< data, owned by the hash
	int				m_iNext;	///< next entry in hash arena
};


template < int SIZE >
class InfixBuilder_c : public ISphInfixBuilder
{
protected:
	static const int							LENGTH = 1048576;

protected:
	int											m_dHash [ LENGTH ];		///< all the hash entries
	CSphSwapVector < InfixHashEntry_t<SIZE> >	m_dArena;
	CSphVector<InfixBlock_t>					m_dBlocks;
	CSphTightVector<BYTE>						m_dBlocksWords;

public:
					InfixBuilder_c();
	virtual void	AddWord ( const BYTE * pWord, int iWordLength, int iCheckpoint );
	virtual void	SaveEntries ( CSphWriter & wrDict );
	virtual int		SaveEntryBlocks ( CSphWriter & wrDict );
	virtual int		GetBlocksWordsSize () const { return m_dBlocksWords.GetLength(); }

protected:
	/// add new entry
	void AddEntry ( const Infix_t<SIZE> & tKey, DWORD uHash, int iCheckpoint )
	{
		uHash &= ( LENGTH-1 );

		int iEntry = m_dArena.GetLength();
		InfixHashEntry_t<SIZE> & tNew = m_dArena.Add();
		tNew.m_tKey = tKey;
		tNew.m_tValue.m_dData[0] = 0x1000000UL | iCheckpoint; // len=1, data=iCheckpoint
		tNew.m_iNext = m_dHash[uHash];
		m_dHash[uHash] = iEntry;
	}

	/// get value pointer by key
	InfixIntvec_t * LookupEntry ( const Infix_t<SIZE> & tKey, DWORD uHash )
	{
		uHash &= ( LENGTH-1 );
		int iEntry = m_dHash [ uHash ];
		int iiEntry = 0;

		while ( iEntry )
		{
			if ( m_dArena[iEntry].m_tKey==tKey )
			{
				// mtf it, if needed
				if ( iiEntry )
				{
					m_dArena[iiEntry].m_iNext = m_dArena[iEntry].m_iNext;
					m_dArena[iEntry].m_iNext = m_dHash[uHash];
					m_dHash[uHash] = iEntry;
				}
				return &m_dArena[iEntry].m_tValue;
			}
			iiEntry = iEntry;
			iEntry = m_dArena[iEntry].m_iNext;
		}
		return NULL;
	}
};


template < int SIZE >
InfixBuilder_c<SIZE>::InfixBuilder_c()
{
	// init the hash
	for ( int i=0; i<LENGTH; i++ )
		m_dHash[i] = 0;
	m_dArena.Reserve ( 1048576 );
	m_dArena.Resize ( 1 ); // 0 is a reserved index
}


/// single-byte case, 2-dword infixes
template<>
void InfixBuilder_c<2>::AddWord ( const BYTE * pWord, int iWordLength, int iCheckpoint )
{
	Infix_t<2> sKey;
	for ( int p=0; p<=iWordLength-2; p++ )
	{
		sKey.Reset();

		BYTE * pKey = (BYTE*)sKey.m_Data;
		const BYTE * s = pWord + p;
		const BYTE * sMax = s + Min ( 6, iWordLength-p );

		DWORD uHash = 0xffffffUL ^ g_dSphinxCRC32 [ 0xff ^ *s ];
		*pKey++ = *s++; // copy first infix byte

		while ( s<sMax )
		{
			uHash = (uHash >> 8) ^ g_dSphinxCRC32 [ (uHash ^ *s) & 0xff ];
			*pKey++ = *s++; // copy another infix byte

			InfixIntvec_t * pVal = LookupEntry ( sKey, uHash );
			if ( pVal )
				pVal->Add ( iCheckpoint );
			else
				AddEntry ( sKey, uHash, iCheckpoint );
		}
	}
}


/// UTF-8 case, 3/5-dword infixes
template < int SIZE >
void InfixBuilder_c<SIZE>::AddWord ( const BYTE * pWord, int iWordLength, int iCheckpoint )
{
	int iCodes = 0; // codepoints in current word
	BYTE dBytes[SPH_MAX_WORD_LEN+1]; // byte offset for each codepoints

	// build an offsets table into the bytestring
	dBytes[0] = 0;
	for ( const BYTE * p = (const BYTE*)pWord; p<pWord+iWordLength; )
	{
		int iLen = 0;
		BYTE uVal = *p;
		while ( uVal & 0x80 )
		{
			uVal <<= 1;
			iLen++;
		}
		if ( !iLen )
			iLen = 1;

		assert ( iLen>=1 && iLen<=4 );
		p += iLen;

		dBytes[iCodes+1] = dBytes[iCodes] + (BYTE)iLen;
		iCodes++;
	}
	assert ( pWord[dBytes[iCodes]]==0 );

	// generate infixes
	Infix_t<SIZE> sKey;
	for ( int p=0; p<=iCodes-2; p++ )
	{
		sKey.Reset();
		BYTE * pKey = (BYTE*)sKey.m_Data;

		const BYTE * s = pWord + dBytes[p];
		const BYTE * sMax = pWord + dBytes[ p+Min ( 6, iCodes-p ) ];

		// copy first infix codepoint
		DWORD uHash = 0xffffffffUL;
		do
		{
			uHash = (uHash >> 8) ^ g_dSphinxCRC32 [ (uHash ^ *s) & 0xff ];
			*pKey++ = *s++;
		} while ( ( *s & 0xC0 )==0x80 );

		while ( s<sMax )
		{
			// copy next infix codepoint
			do
			{
				uHash = (uHash >> 8) ^ g_dSphinxCRC32 [ (uHash ^ *s) & 0xff ];
				*pKey++ = *s++;
			} while ( ( *s & 0xC0 )==0x80 );

			InfixIntvec_t * pVal = LookupEntry ( sKey, uHash );
			if ( pVal )
				pVal->Add ( iCheckpoint );
			else
				AddEntry ( sKey, uHash, iCheckpoint );
		}
	}
}


template < int SIZE >
struct InfixHashCmp_fn
{
	InfixHashEntry_t<SIZE> * m_pBase;

	explicit InfixHashCmp_fn ( InfixHashEntry_t<SIZE> * pBase )
		: m_pBase ( pBase )
	{}

	bool IsLess ( int a, int b ) const
	{
		return strncmp ( (const char*)m_pBase[a].m_tKey.m_Data, (const char*)m_pBase[b].m_tKey.m_Data, sizeof(DWORD)*SIZE )<0;
	}
};


static inline int ZippedIntSize ( DWORD v )
{
	if ( v < (1UL<<7) )
		return 1;
	if ( v < (1UL<<14) )
		return 2;
	if ( v < (1UL<<21) )
		return 3;
	if ( v < (1UL<<28) )
		return 4;
	return 5;
}


static const char * g_sTagInfixEntries = "infix-entries";

template < int SIZE >
void InfixBuilder_c<SIZE>::SaveEntries ( CSphWriter & wrDict )
{
	// intentionally local to this function
	// we mark the block end with an editcode of 0
	const int INFIX_BLOCK_SIZE = 64;

	wrDict.PutBytes ( g_sTagInfixEntries, strlen ( g_sTagInfixEntries ) );

	CSphVector<int> dIndex;
	dIndex.Resize ( m_dArena.GetLength()-1 );
	for ( int i=0; i<m_dArena.GetLength()-1; i++ )
		dIndex[i] = i+1;

	InfixHashCmp_fn<SIZE> fnCmp ( m_dArena.Begin() );
	dIndex.Sort ( fnCmp );

	m_dBlocksWords.Reserve ( m_dArena.GetLength()/INFIX_BLOCK_SIZE*sizeof(DWORD)*SIZE );
	int iBlock = 0;
	int iPrevKey = -1;
	ARRAY_FOREACH ( iIndex, dIndex )
	{
		InfixIntvec_t & dData = m_dArena[dIndex[iIndex]].m_tValue;
		const BYTE * sKey = (const BYTE*) m_dArena[dIndex[iIndex]].m_tKey.m_Data;
		int iChars = ( SIZE==2 )
			? strnlen ( (const char*)sKey, sizeof(DWORD)*SIZE )
			: sphUTF8Len ( (const char*)sKey, sizeof(DWORD)*SIZE );
		assert ( iChars>=2 && iChars<int(1 + sizeof ( Infix_t<SIZE> ) ) );

		// keep track of N-infix blocks
		int iAppendBytes = strnlen ( (const char*)sKey, sizeof(DWORD)*SIZE );
		if ( !iBlock )
		{
			int iOff = m_dBlocksWords.GetLength();
			m_dBlocksWords.Resize ( iOff+iAppendBytes+1 );

			InfixBlock_t & tBlock = m_dBlocks.Add();
			tBlock.m_iInfixOffset = iOff;
			tBlock.m_iOffset = (int)wrDict.GetPos();

			memcpy ( m_dBlocksWords.Begin()+iOff, sKey, iAppendBytes );
			m_dBlocksWords[iOff+iAppendBytes] = '\0';
		}

		// compute max common prefix
		// edit_code = ( num_keep_chars<<4 ) + num_append_chars
		int iEditCode = iChars;
		if ( iPrevKey>=0 )
		{
			const BYTE * sPrev = (const BYTE*) m_dArena[dIndex[iPrevKey]].m_tKey.m_Data;
			const BYTE * sCur = (const BYTE*) sKey;
			const BYTE * sMax = sCur + iAppendBytes;

			int iKeepChars = 0;
			if_const ( SIZE==2 )
			{
				// SBCS path
				while ( sCur<sMax && *sCur && *sCur==*sPrev )
				{
					sCur++;
					sPrev++;
				}
				iKeepChars = (int)( sCur- ( const BYTE* ) sKey );

				assert ( iKeepChars>=0 && iKeepChars<16 );
				assert ( iChars-iKeepChars>=0 );
				assert ( iChars-iKeepChars<16 );

				iEditCode = ( iKeepChars<<4 ) + ( iChars-iKeepChars );
				iAppendBytes = ( iChars-iKeepChars );
				sKey = sCur;

			} else
			{
				// UTF-8 path
				const BYTE * sKeyMax = sCur; // track max matching sPrev prefix in [sKey,sKeyMax)
				while ( sCur<sMax && *sCur && *sCur==*sPrev )
				{
					// current byte matches, move the pointer
					sCur++;
					sPrev++;

					// tricky bit
					// if the next (!) byte is a valid UTF-8 char start (or eof!)
					// then we just matched not just a byte, but a full char
					// so bump the matching prefix boundary and length
					if ( sCur>=sMax || ( *sCur & 0xC0 )!=0x80 )
					{
						sKeyMax = sCur;
						iKeepChars++;
					}
				}

				assert ( iKeepChars>=0 && iKeepChars<16 );
				assert ( iChars-iKeepChars>=0 );
				assert ( iChars-iKeepChars<16 );

				iEditCode = ( iKeepChars<<4 ) + ( iChars-iKeepChars );
				iAppendBytes -= (int)( sKeyMax-sKey );
				sKey = sKeyMax;
			}
		}

		// write edit code, postfix
		wrDict.PutByte ( iEditCode );
		wrDict.PutBytes ( sKey, iAppendBytes );

		// compute data length
		int iDataLen = ZippedIntSize ( dData[0] );
		for ( int j=1; j<dData.GetLength(); j++ )
			iDataLen += ZippedIntSize ( dData[j] - dData[j-1] );

		// write data length, data
		wrDict.ZipInt ( iDataLen );
		wrDict.ZipInt ( dData[0] );
		for ( int j=1; j<dData.GetLength(); j++ )
			wrDict.ZipInt ( dData[j] - dData[j-1] );

		// mark block end, restart deltas
		iPrevKey = iIndex;
		if ( ++iBlock==INFIX_BLOCK_SIZE )
		{
			iBlock = 0;
			iPrevKey = -1;
			wrDict.PutByte ( 0 );
		}
	}

	// put end marker
	if ( iBlock )
		wrDict.PutByte ( 0 );

	const char * pBlockWords = (const char *)m_dBlocksWords.Begin();
	ARRAY_FOREACH ( i, m_dBlocks )
		m_dBlocks[i].m_sInfix = pBlockWords+m_dBlocks[i].m_iInfixOffset;
}


static const char * g_sTagInfixBlocks = "infix-blocks";

template < int SIZE >
int InfixBuilder_c<SIZE>::SaveEntryBlocks ( CSphWriter & wrDict )
{
	// save the blocks
	wrDict.PutBytes ( g_sTagInfixBlocks, strlen ( g_sTagInfixBlocks ) );

	SphOffset_t iInfixBlocksOffset = wrDict.GetPos();
	assert ( iInfixBlocksOffset<=INT_MAX );

	wrDict.ZipInt ( m_dBlocks.GetLength() );
	ARRAY_FOREACH ( i, m_dBlocks )
	{
		int iBytes = strlen ( m_dBlocks[i].m_sInfix );
		wrDict.PutByte ( iBytes );
		wrDict.PutBytes ( m_dBlocks[i].m_sInfix, iBytes );
		wrDict.ZipInt ( m_dBlocks[i].m_iOffset ); // maybe delta these on top?
	}

	return (int)iInfixBlocksOffset;
}


ISphInfixBuilder * sphCreateInfixBuilder ( int iCodepointBytes, CSphString * pError )
{
	assert ( pError );
	*pError = CSphString();
	switch ( iCodepointBytes )
	{
	case 0:		return NULL;
	case 1:		return new InfixBuilder_c<2>(); // upto 6x1 bytes, 2 dwords, sbcs
	case 2:		return new InfixBuilder_c<3>(); // upto 6x2 bytes, 3 dwords, utf-8
	case 3:		return new InfixBuilder_c<5>(); // upto 6x3 bytes, 5 dwords, utf-8
	default:	pError->SetSprintf ( "unhandled max infix codepoint size %d", iCodepointBytes ); return NULL;
	}
}

//////////////////////////////////////////////////////////////////////////
// KEYWORDS STORING DICTIONARY
//////////////////////////////////////////////////////////////////////////

class CSphDictKeywords : public CSphDictCRC<true>
{
private:
	static const int				SLOTS			= 65536;
	static const int				ENTRY_CHUNK		= 65536;
	static const int				KEYWORD_CHUNK	= 1048576;
	static const int				DICT_CHUNK		= 65536;

public:
	// OPTIMIZE? change pointers to 8:24 locators to save RAM on x64 gear?
	struct HitblockKeyword_t
	{
		SphWordID_t					m_uWordid;			// locally unique word id (crc value, adjusted in case of collsion)
		HitblockKeyword_t *			m_pNextHash;		// next hashed entry
		char *						m_pKeyword;			// keyword
	};

	struct HitblockException_t
	{
		HitblockKeyword_t *			m_pEntry;			// hash entry
		SphWordID_t					m_uCRC;				// original unadjusted crc

		bool operator < ( const HitblockException_t & rhs ) const
		{
			return m_pEntry->m_uWordid < rhs.m_pEntry->m_uWordid;
		}
	};

	struct DictKeyword_t
	{
		char *						m_sKeyword;
		SphOffset_t					m_uOff;
		int							m_iDocs;
		int							m_iHits;
		BYTE						m_uHint;
		int							m_iSkiplistPos;		///< position in .spe file; not exactly likely to hit 2B
	};

	struct DictBlock_t
	{
		SphOffset_t					m_iPos;
		int							m_iLen;
	};

private:
	HitblockKeyword_t *				m_dHash [ SLOTS ];	///< hash by wordid (!)
	CSphVector<HitblockException_t>	m_dExceptions;

	bool							m_bHitblock;		///< should we store words on GetWordID or not
	int								m_iMemUse;			///< current memory use by all the chunks
	int								m_iDictLimit;		///< allowed memory limit for dict block collection

	CSphVector<HitblockKeyword_t*>	m_dEntryChunks;		///< hash chunks, only used when indexing hitblocks
	HitblockKeyword_t *				m_pEntryChunk;
	int								m_iEntryChunkFree;

	CSphVector<BYTE*>				m_dKeywordChunks;	///< keyword storage
	BYTE *							m_pKeywordChunk;
	int								m_iKeywordChunkFree;

	CSphVector<DictKeyword_t*>		m_dDictChunks;		///< dict entry chunks, only used when sorting final dict
	DictKeyword_t *					m_pDictChunk;
	int								m_iDictChunkFree;

	int								m_iTmpFD;			///< temp dict file descriptor
	CSphWriter						m_wrTmpDict;		///< temp dict writer
	CSphVector<DictBlock_t>			m_dDictBlocks;		///< on-disk locations of dict entry blocks

	char							m_sClippedWord[MAX_KEYWORD_BYTES]; ///< keyword storage for cliiped word

private:
	SphWordID_t						HitblockGetID ( const char * pWord, int iLen, SphWordID_t uCRC );
	HitblockKeyword_t *				HitblockAddKeyword ( DWORD uHash, const char * pWord, int iLen, SphWordID_t uID );

public:
	explicit				CSphDictKeywords ();
	virtual					~CSphDictKeywords ();

	virtual void			HitblockBegin () { m_bHitblock = true; }
	virtual void			HitblockPatch ( CSphWordHit * pHits, int iHits ) const;
	virtual const char *	HitblockGetKeyword ( SphWordID_t uWordID );
	virtual int				HitblockGetMemUse () { return m_iMemUse; }
	virtual void			HitblockReset ();

	virtual void			DictBegin ( CSphAutofile & tTempDict, CSphAutofile & tDict, int iDictLimit, ThrottleState_t * pThrottle );
	virtual void			DictEntry ( const CSphDictEntry & tEntry );
	virtual void			DictEndEntries ( SphOffset_t ) {}
	virtual bool			DictEnd ( DictHeader_t * pHeader, int iMemLimit, CSphString & sError, ThrottleState_t * pThrottle );

	virtual SphWordID_t		GetWordID ( BYTE * pWord );
	virtual SphWordID_t		GetWordIDWithMarkers ( BYTE * pWord );
	virtual SphWordID_t		GetWordIDNonStemmed ( BYTE * pWord );
	virtual SphWordID_t		GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops );
	virtual CSphDict *		Clone () const { return CloneBase ( new CSphDictKeywords() ); }

private:
	void					DictFlush ();
};

//////////////////////////////////////////////////////////////////////////

CSphDictKeywords::CSphDictKeywords ()
	: m_bHitblock ( false )
	, m_iMemUse ( 0 )
	, m_iDictLimit ( 0 )
	, m_pEntryChunk ( NULL )
	, m_iEntryChunkFree ( 0 )
	, m_pKeywordChunk ( NULL )
	, m_iKeywordChunkFree ( 0 )
	, m_pDictChunk ( NULL )
	, m_iDictChunkFree ( 0 )
{
	memset ( m_dHash, 0, sizeof(m_dHash) );
}

CSphDictKeywords::~CSphDictKeywords ()
{
	HitblockReset();
}

void CSphDictKeywords::HitblockReset()
{
	m_dExceptions.Resize ( 0 );

	ARRAY_FOREACH ( i, m_dEntryChunks )
		SafeDeleteArray ( m_dEntryChunks[i] );
	m_dEntryChunks.Resize ( 0 );
	m_pEntryChunk = NULL;
	m_iEntryChunkFree = 0;

	ARRAY_FOREACH ( i, m_dKeywordChunks )
		SafeDeleteArray ( m_dKeywordChunks[i] );
	m_dKeywordChunks.Resize ( 0 );
	m_pKeywordChunk = NULL;
	m_iKeywordChunkFree = 0;

	m_iMemUse = 0;

	memset ( m_dHash, 0, sizeof(m_dHash) );
}

CSphDictKeywords::HitblockKeyword_t * CSphDictKeywords::HitblockAddKeyword ( DWORD uHash, const char * sWord, int iLen, SphWordID_t uID )
{
	assert ( iLen<MAX_KEYWORD_BYTES );

	// alloc entry
	if ( !m_iEntryChunkFree )
	{
		m_pEntryChunk = new HitblockKeyword_t [ ENTRY_CHUNK ];
		m_iEntryChunkFree = ENTRY_CHUNK;
		m_dEntryChunks.Add ( m_pEntryChunk );
		m_iMemUse += sizeof(HitblockKeyword_t)*ENTRY_CHUNK;
	}
	HitblockKeyword_t * pEntry = m_pEntryChunk++;
	m_iEntryChunkFree--;

	// alloc keyword
	iLen++;
	if ( m_iKeywordChunkFree < iLen )
	{
		m_pKeywordChunk = new BYTE [ KEYWORD_CHUNK ];
		m_iKeywordChunkFree = KEYWORD_CHUNK;
		m_dKeywordChunks.Add ( m_pKeywordChunk );
		m_iMemUse += KEYWORD_CHUNK;
	}

	// fill it
	memcpy ( m_pKeywordChunk, sWord, iLen );
	m_pKeywordChunk[iLen-1] = '\0';
	pEntry->m_pKeyword = (char*)m_pKeywordChunk;
	pEntry->m_uWordid = uID;
	m_pKeywordChunk += iLen;
	m_iKeywordChunkFree -= iLen;

	// mtf it
	pEntry->m_pNextHash = m_dHash [ uHash ];
	m_dHash [ uHash ] = pEntry;

	return pEntry;
}

SphWordID_t CSphDictKeywords::HitblockGetID ( const char * sWord, int iLen, SphWordID_t uCRC )
{
	if ( iLen>=MAX_KEYWORD_BYTES-4 ) // fix of very long word (zones)
	{
		memcpy ( m_sClippedWord, sWord, MAX_KEYWORD_BYTES-4 );
		memset ( m_sClippedWord+MAX_KEYWORD_BYTES-4, 0, 4 );

		CSphString sOrig;
		sOrig.SetBinary ( sWord, iLen );
		sphWarn ( "word overrun buffer, clipped!!!\n"
			"clipped (len=%d, word='%s')\noriginal (len=%d, word='%s')",
			MAX_KEYWORD_BYTES-4, m_sClippedWord, iLen, sOrig.cstr() );

		sWord = m_sClippedWord;
		iLen = MAX_KEYWORD_BYTES-4;
		uCRC = sphCRC32 ( (const BYTE *)m_sClippedWord, MAX_KEYWORD_BYTES-4 );
	}

	// is this a known one? find it
	// OPTIMIZE? in theory we could use something faster than crc32; but quick lookup3 test did not show any improvements
	const DWORD uHash = (DWORD)( uCRC % SLOTS );

	HitblockKeyword_t * pEntry = m_dHash [ uHash ];
	HitblockKeyword_t ** ppEntry = &m_dHash [ uHash ];
	while ( pEntry )
	{
		// check crc
		if ( pEntry->m_uWordid!=uCRC )
		{
			// crc mismatch, try next entry
			ppEntry = &pEntry->m_pNextHash;
			pEntry = pEntry->m_pNextHash;
			continue;
		}

		// crc matches, check keyword
		register int iWordLen = iLen;
		register const char * a = pEntry->m_pKeyword;
		register const char * b = sWord;
		while ( *a==*b && iWordLen-- )
		{
			if ( !*a || !iWordLen )
			{
				// known word, mtf it, and return id
				(*ppEntry) = pEntry->m_pNextHash;
				pEntry->m_pNextHash = m_dHash [ uHash ];
				m_dHash [ uHash ] = pEntry;
				return pEntry->m_uWordid;
			}
			a++;
			b++;
		}

		// collision detected!
		// our crc is taken as a wordid, but keyword does not match
		// welcome to the land of very tricky magic
		//
		// pEntry might either be a known exception, or a regular keyword
		// sWord might either be a known exception, or a new one
		// if they are not known, they needed to be added as exceptions now
		//
		// in case sWord is new, we need to assign a new unique wordid
		// for that, we keep incrementing the crc until it is unique
		// a starting point for wordid search loop would be handy
		//
		// let's scan the exceptions vector and work on all this
		//
		// NOTE, beware of the order, it is wordid asc, which does NOT guarantee crc asc
		// example, assume crc(w1)==X, crc(w2)==X+1, crc(w3)==X (collides with w1)
		// wordids will be X, X+1, X+2 but crcs will be X, X+1, X
		//
		// OPTIMIZE, might make sense to use binary search
		// OPTIMIZE, add early out somehow
		SphWordID_t uWordid = uCRC + 1;
		const int iExcLen = m_dExceptions.GetLength();
		int iExc = m_dExceptions.GetLength();
		ARRAY_FOREACH ( i, m_dExceptions )
		{
			const HitblockKeyword_t * pExcWord = m_dExceptions[i].m_pEntry;

			// incoming word is a known exception? just return the pre-assigned wordid
			if ( m_dExceptions[i].m_uCRC==uCRC && strncmp ( pExcWord->m_pKeyword, sWord, iLen )==0 )
				return pExcWord->m_uWordid;

			// incoming word collided into a known exception? clear the matched entry; no need to re-add it (see below)
			if ( pExcWord==pEntry )
				pEntry = NULL;

			// find first exception with wordid greater or equal to our candidate
			if ( pExcWord->m_uWordid>=uWordid && iExc==iExcLen )
				iExc = i;
		}

		// okay, this is a new collision
		// if entry was a regular word, we have to add it
		if ( pEntry )
		{
			m_dExceptions.Add();
			m_dExceptions.Last().m_pEntry = pEntry;
			m_dExceptions.Last().m_uCRC = uCRC;
		}

		// need to assign a new unique wordid now
		// keep scanning both exceptions and keywords for collisions
		for ( ;; )
		{
			// iExc must be either the first exception greater or equal to current candidate, or out of bounds
			assert ( iExc==iExcLen || m_dExceptions[iExc].m_pEntry->m_uWordid>=uWordid );
			assert ( iExc==0 || m_dExceptions[iExc-1].m_pEntry->m_uWordid<uWordid );

			// candidate collides with a known exception? increment it, and keep looking
			if ( iExc<iExcLen && m_dExceptions[iExc].m_pEntry->m_uWordid==uWordid )
			{
				uWordid++;
				while ( iExc<iExcLen && m_dExceptions[iExc].m_pEntry->m_uWordid<uWordid )
					iExc++;
				continue;
			}

			// candidate collides with a keyword? must be a regular one; add it as an exception, and keep looking
			HitblockKeyword_t * pCheck = m_dHash [ (DWORD)( uWordid % SLOTS ) ];
			while ( pCheck )
			{
				if ( pCheck->m_uWordid==uWordid )
					break;
				pCheck = pCheck->m_pNextHash;
			}

			// no collisions; we've found our unique wordid!
			if ( !pCheck )
				break;

			// got a collision; add it
			HitblockException_t & tColl = m_dExceptions.Add();
			tColl.m_pEntry = pCheck;
			tColl.m_uCRC = pCheck->m_uWordid; // not a known exception; hence, wordid must equal crc

			// and keep looking
			uWordid++;
			continue;
		}

		// and finally, we have that precious new wordid
		// so hash our new unique under its new unique adjusted wordid
		pEntry = HitblockAddKeyword ( (DWORD)( uWordid % SLOTS ), sWord, iLen, uWordid );

		// add it as a collision too
		m_dExceptions.Add();
		m_dExceptions.Last().m_pEntry = pEntry;
		m_dExceptions.Last().m_uCRC = uCRC;

		// keep exceptions list sorted by wordid
		m_dExceptions.Sort();

		return pEntry->m_uWordid;
	}

	// new keyword with unique crc
	pEntry = HitblockAddKeyword ( uHash, sWord, iLen, uCRC );
	return pEntry->m_uWordid;
}

struct DictKeywordTagged_t : public CSphDictKeywords::DictKeyword_t
{
	int m_iBlock;
};

struct DictKeywordTaggedCmp_fn
{
	static inline bool IsLess ( const DictKeywordTagged_t & a, const DictKeywordTagged_t & b )
	{
		return strcmp ( a.m_sKeyword, b.m_sKeyword ) < 0;
	}
};

static void DictReadEntry ( CSphBin * pBin, DictKeywordTagged_t & tEntry, BYTE * pKeyword )
{
	int iKeywordLen = pBin->ReadByte ();
	if ( iKeywordLen<0 )
	{
		// early eof or read error; flag must be raised
		assert ( pBin->IsError() );
		return;
	}

	assert ( iKeywordLen>0 && iKeywordLen<MAX_KEYWORD_BYTES-1 );
	if ( pBin->ReadBytes ( pKeyword, iKeywordLen )<0 )
	{
		assert ( pBin->IsError() );
		return;
	}
	pKeyword[iKeywordLen] = '\0';

	tEntry.m_sKeyword = (char*)pKeyword;
	tEntry.m_uOff = pBin->UnzipOffset();
	tEntry.m_iDocs = pBin->UnzipInt();
	tEntry.m_iHits = pBin->UnzipInt();
	tEntry.m_uHint = (BYTE) pBin->ReadByte();
	if ( tEntry.m_iDocs > SPH_SKIPLIST_BLOCK )
		tEntry.m_iSkiplistPos = pBin->UnzipInt();
	else
		tEntry.m_iSkiplistPos = 0;
}

void CSphDictKeywords::DictBegin ( CSphAutofile & tTempDict, CSphAutofile & tDict, int iDictLimit, ThrottleState_t * pThrottle )
{
	m_iTmpFD = tTempDict.GetFD();
	m_wrTmpDict.CloseFile ();
	m_wrTmpDict.SetFile ( tTempDict, NULL, m_sWriterError );
	m_wrTmpDict.SetThrottle ( pThrottle );

	m_wrDict.CloseFile ();
	m_wrDict.SetFile ( tDict, NULL, m_sWriterError );
	m_wrDict.SetThrottle ( pThrottle );
	m_wrDict.PutByte ( 1 );

	m_iDictLimit = Max ( iDictLimit, KEYWORD_CHUNK + DICT_CHUNK*(int)sizeof(DictKeyword_t) ); // can't use less than 1 chunk
}


bool CSphDictKeywords::DictEnd ( DictHeader_t * pHeader, int iMemLimit, CSphString & sError, ThrottleState_t * pThrottle )
{
	DictFlush ();
	m_wrTmpDict.CloseFile (); // tricky: file is not owned, so it won't get closed, and iTmpFD won't get invalidated

	if ( !m_dDictBlocks.GetLength() )
		m_wrDict.CloseFile();

	if ( m_wrTmpDict.IsError() || m_wrDict.IsError() )
	{
		sError.SetSprintf ( "dictionary write error (out of space?)" );
		return false;
	}

	if ( !m_dDictBlocks.GetLength() )
	{
		pHeader->m_iDictCheckpointsOffset = m_wrDict.GetPos ();
		pHeader->m_iDictCheckpoints = 0;
		return true;
	}

	// infix builder, if needed
	ISphInfixBuilder * pInfixer = sphCreateInfixBuilder ( pHeader->m_iInfixCodepointBytes, &sError );
	if ( !sError.IsEmpty() )
	{
		SafeDelete ( pInfixer );
		return false;
	}

	// initialize readers
	CSphVector<CSphBin*> dBins ( m_dDictBlocks.GetLength() );

	int iMaxBlock = 0;
	ARRAY_FOREACH ( i, m_dDictBlocks )
		iMaxBlock = Max ( iMaxBlock, m_dDictBlocks[i].m_iLen );

	iMemLimit = Max ( iMemLimit, iMaxBlock*m_dDictBlocks.GetLength() );
	int iBinSize = CSphBin::CalcBinSize ( iMemLimit, m_dDictBlocks.GetLength(), "sort_dict" );

	SphOffset_t iSharedOffset = -1;
	ARRAY_FOREACH ( i, m_dDictBlocks )
	{
		dBins[i] = new CSphBin();
		dBins[i]->m_iFileLeft = m_dDictBlocks[i].m_iLen;
		dBins[i]->m_iFilePos = m_dDictBlocks[i].m_iPos;
		dBins[i]->Init ( m_iTmpFD, &iSharedOffset, iBinSize );
		dBins[i]->SetThrottle ( pThrottle );
	}

	// keywords storage
	BYTE * pKeywords = new BYTE [ MAX_KEYWORD_BYTES*dBins.GetLength() ];

	#define LOC_CLEANUP() \
		{ \
			ARRAY_FOREACH ( iIdx, dBins ) \
				SafeDelete ( dBins[iIdx] ); \
			SafeDeleteArray ( pKeywords ); \
			SafeDelete ( pInfixer ); \
		}

	// do the sort
	CSphQueue < DictKeywordTagged_t, DictKeywordTaggedCmp_fn > qWords ( dBins.GetLength() );
	DictKeywordTagged_t tEntry;

	ARRAY_FOREACH ( i, dBins )
	{
		DictReadEntry ( dBins[i], tEntry, pKeywords + i*MAX_KEYWORD_BYTES );
		if ( dBins[i]->IsError() )
		{
			sError.SetSprintf ( "entry read error in dictionary sort (bin %d of %d)", i, dBins.GetLength() );
			LOC_CLEANUP();
			return false;
		}

		tEntry.m_iBlock = i;
		qWords.Push ( tEntry );
	}

	CSphKeywordDeltaWriter tLastKeyword;
	int iWords = 0;
	while ( qWords.GetLength() )
	{
		const DictKeywordTagged_t & tWord = qWords.Root();
		const int iLen = strlen ( tWord.m_sKeyword ); // OPTIMIZE?

		// store checkpoints as needed
		if ( ( iWords % SPH_WORDLIST_CHECKPOINT )==0 )
		{
			// emit a checkpoint, unless we're at the very dict beginning
			if ( iWords )
			{
				m_wrDict.ZipInt ( 0 );
				m_wrDict.ZipInt ( 0 );
			}

			BYTE * sClone = new BYTE [ iLen+1 ]; // OPTIMIZE? pool these?
			memcpy ( sClone, tWord.m_sKeyword, iLen+1 );
			sClone[iLen] = '\0';

			CSphWordlistCheckpoint & tCheckpoint = m_dCheckpoints.Add ();
			tCheckpoint.m_sWord = (char*) sClone;
			tCheckpoint.m_iWordlistOffset = m_wrDict.GetPos();

			tLastKeyword.Reset();
		}
		iWords++;

		// write final dict entry
		assert ( iLen );
		assert ( tWord.m_uOff );
		assert ( tWord.m_iDocs );
		assert ( tWord.m_iHits );

		tLastKeyword.PutDelta ( m_wrDict, (const BYTE *)tWord.m_sKeyword, iLen );
		m_wrDict.ZipOffset ( tWord.m_uOff );
		m_wrDict.ZipInt ( tWord.m_iDocs );
		m_wrDict.ZipInt ( tWord.m_iHits );
		if ( tWord.m_uHint )
			m_wrDict.PutByte ( tWord.m_uHint );
		if ( tWord.m_iDocs > SPH_SKIPLIST_BLOCK )
			m_wrDict.ZipInt ( tWord.m_iSkiplistPos );

		// build infixes
		if ( pInfixer )
			pInfixer->AddWord ( (const BYTE*)tWord.m_sKeyword, iLen, m_dCheckpoints.GetLength() );

		// next
		int iBin = tWord.m_iBlock;
		qWords.Pop ();

		if ( !dBins[iBin]->IsDone() )
		{
			DictReadEntry ( dBins[iBin], tEntry, pKeywords + iBin*MAX_KEYWORD_BYTES );
			if ( dBins[iBin]->IsError() )
			{
				sError.SetSprintf ( "entry read error in dictionary sort (bin %d of %d)", iBin, dBins.GetLength() );
				LOC_CLEANUP();
				return false;
			}

			tEntry.m_iBlock = iBin;
			qWords.Push ( tEntry );
		}
	}

	// end of dictionary block
	m_wrDict.ZipInt ( 0 );
	m_wrDict.ZipInt ( 0 );

	// flush infix hash entries, if any
	if ( pInfixer )
		pInfixer->SaveEntries ( m_wrDict );

	// flush wordlist checkpoints (blocks)
	pHeader->m_iDictCheckpointsOffset = m_wrDict.GetPos();
	pHeader->m_iDictCheckpoints = m_dCheckpoints.GetLength();

	ARRAY_FOREACH ( i, m_dCheckpoints )
	{
		const int iLen = strlen ( m_dCheckpoints[i].m_sWord );

		assert ( m_dCheckpoints[i].m_iWordlistOffset>0 );
		assert ( iLen>0 && iLen<MAX_KEYWORD_BYTES );

		m_wrDict.PutDword ( iLen );
		m_wrDict.PutBytes ( m_dCheckpoints[i].m_sWord, iLen );
		m_wrDict.PutOffset ( m_dCheckpoints[i].m_iWordlistOffset );

		SafeDeleteArray ( m_dCheckpoints[i].m_sWord );
	}

	// flush infix hash blocks
	if ( pInfixer )
	{
		pHeader->m_iInfixBlocksOffset = pInfixer->SaveEntryBlocks ( m_wrDict );
		pHeader->m_iInfixBlocksWordsSize = pInfixer->GetBlocksWordsSize();
	}

	// flush header
	// mostly for debugging convenience
	// primary storage is in the index wide header
	m_wrDict.PutBytes ( "dict-header", 11 );
	m_wrDict.ZipInt ( pHeader->m_iDictCheckpoints );
	m_wrDict.ZipOffset ( pHeader->m_iDictCheckpointsOffset );
	m_wrDict.ZipInt ( pHeader->m_iInfixCodepointBytes );
	m_wrDict.ZipInt ( pHeader->m_iInfixBlocksOffset );

	// about it
	LOC_CLEANUP();
	#undef LOC_CLEANUP

	m_wrDict.CloseFile ();
	if ( m_wrDict.IsError() )
		sError.SetSprintf ( "dictionary write error (out of space?)" );
	return !m_wrDict.IsError();
}

struct DictKeywordCmp_fn
{
	inline bool IsLess ( CSphDictKeywords::DictKeyword_t * a, CSphDictKeywords::DictKeyword_t * b ) const
	{
		return strcmp ( a->m_sKeyword, b->m_sKeyword ) < 0;
	}
};

void CSphDictKeywords::DictFlush ()
{
	if ( !m_dDictChunks.GetLength() )
		return;
	assert ( m_dDictChunks.GetLength() && m_dKeywordChunks.GetLength() );

	// sort em
	int iTotalWords = m_dDictChunks.GetLength()*DICT_CHUNK - m_iDictChunkFree;
	CSphVector<DictKeyword_t*> dWords ( iTotalWords );

	int iIdx = 0;
	ARRAY_FOREACH ( i, m_dDictChunks )
	{
		int iWords = DICT_CHUNK;
		if ( i==m_dDictChunks.GetLength()-1 )
			iWords -= m_iDictChunkFree;

		DictKeyword_t * pWord = m_dDictChunks[i];
		for ( int j=0; j<iWords; j++ )
			dWords[iIdx++] = pWord++;
	}

	dWords.Sort ( DictKeywordCmp_fn() );

	// write em
	DictBlock_t & tBlock = m_dDictBlocks.Add();
	tBlock.m_iPos = m_wrTmpDict.GetPos ();

	ARRAY_FOREACH ( i, dWords )
	{
		const DictKeyword_t * pWord = dWords[i];
		int iLen = strlen ( pWord->m_sKeyword );
		m_wrTmpDict.PutByte ( iLen );
		m_wrTmpDict.PutBytes ( pWord->m_sKeyword, iLen );
		m_wrTmpDict.ZipOffset ( pWord->m_uOff );
		m_wrTmpDict.ZipInt ( pWord->m_iDocs );
		m_wrTmpDict.ZipInt ( pWord->m_iHits );
		m_wrTmpDict.PutByte ( pWord->m_uHint );
		assert ( ( pWord->m_iDocs > SPH_SKIPLIST_BLOCK )==( pWord->m_iSkiplistPos!=0 ) );
		if ( pWord->m_iDocs > SPH_SKIPLIST_BLOCK )
			m_wrTmpDict.ZipInt ( pWord->m_iSkiplistPos );
	}

	tBlock.m_iLen = (int)( m_wrTmpDict.GetPos() - tBlock.m_iPos );

	// clean up buffers
	ARRAY_FOREACH ( i, m_dDictChunks )
		SafeDeleteArray ( m_dDictChunks[i] );
	m_dDictChunks.Resize ( 0 );
	m_pDictChunk = NULL;
	m_iDictChunkFree = 0;

	ARRAY_FOREACH ( i, m_dKeywordChunks )
		SafeDeleteArray ( m_dKeywordChunks[i] );
	m_dKeywordChunks.Resize ( 0 );
	m_pKeywordChunk = NULL;
	m_iKeywordChunkFree = 0;

	m_iMemUse = 0;
}

void CSphDictKeywords::DictEntry ( const CSphDictEntry & tEntry )
{
	// they say, this might just happen during merge
	// FIXME! can we make merge avoid sending such keywords to dict and assert here?
	if ( !tEntry.m_iDocs )
		return;

	assert ( tEntry.m_iHits );
	assert ( tEntry.m_iDoclistLength>0 );

	DictKeyword_t * pWord = NULL;
	int iLen = strlen ( (char*)tEntry.m_sKeyword ) + 1;

	for ( ;; )
	{
		// alloc dict entry
		if ( !m_iDictChunkFree )
		{
			if ( m_iDictLimit && ( m_iMemUse + (int)sizeof(DictKeyword_t)*DICT_CHUNK )>m_iDictLimit )
				DictFlush ();

			m_pDictChunk = new DictKeyword_t [ DICT_CHUNK ];
			m_iDictChunkFree = DICT_CHUNK;
			m_dDictChunks.Add ( m_pDictChunk );
			m_iMemUse += sizeof(DictKeyword_t)*DICT_CHUNK;
		}

		// alloc keyword
		if ( m_iKeywordChunkFree < iLen )
		{
			if ( m_iDictLimit && ( m_iMemUse + KEYWORD_CHUNK )>m_iDictLimit )
			{
				DictFlush ();
				continue; // because we just flushed pWord
			}

			m_pKeywordChunk = new BYTE [ KEYWORD_CHUNK ];
			m_iKeywordChunkFree = KEYWORD_CHUNK;
			m_dKeywordChunks.Add ( m_pKeywordChunk );
			m_iMemUse += KEYWORD_CHUNK;
		}
		// aw kay
		break;
	}

	pWord = m_pDictChunk++;
	m_iDictChunkFree--;
	pWord->m_sKeyword = (char*)m_pKeywordChunk;
	memcpy ( m_pKeywordChunk, tEntry.m_sKeyword, iLen );
	m_pKeywordChunk[iLen-1] = '\0';
	m_pKeywordChunk += iLen;
	m_iKeywordChunkFree -= iLen;

	pWord->m_uOff = tEntry.m_iDoclistOffset;
	pWord->m_iDocs = tEntry.m_iDocs;
	pWord->m_iHits = tEntry.m_iHits;
	pWord->m_uHint = sphDoclistHintPack ( tEntry.m_iDocs, tEntry.m_iDoclistLength );
	pWord->m_iSkiplistPos = 0;
	if ( tEntry.m_iDocs > SPH_SKIPLIST_BLOCK )
		pWord->m_iSkiplistPos = (int)( tEntry.m_iSkiplistOffset );
}

SphWordID_t CSphDictKeywords::GetWordID ( BYTE * pWord )
{
	SphWordID_t uCRC = CSphDictCRC<true>::GetWordID ( pWord );
	if ( !uCRC || !m_bHitblock )
		return uCRC;

	int iLen = strlen ( (const char *)pWord );
	return HitblockGetID ( (const char *)pWord, iLen, uCRC );
}

SphWordID_t CSphDictKeywords::GetWordIDWithMarkers ( BYTE * pWord )
{
	SphWordID_t uCRC = CSphDictCRC<true>::GetWordIDWithMarkers ( pWord );
	if ( !uCRC || !m_bHitblock )
		return uCRC;

	int iLen = strlen ( (const char *)pWord );
	return HitblockGetID ( (const char *)pWord, iLen, uCRC );
}

SphWordID_t CSphDictKeywords::GetWordIDNonStemmed ( BYTE * pWord )
{
	SphWordID_t uCRC = CSphDictCRC<true>::GetWordIDNonStemmed ( pWord );
	if ( !uCRC || !m_bHitblock )
		return uCRC;

	int iLen = strlen ( (const char *)pWord );
	return HitblockGetID ( (const char *)pWord, iLen, uCRC );
}

SphWordID_t CSphDictKeywords::GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops )
{
	SphWordID_t uCRC = CSphDictCRC<true>::GetWordID ( pWord, iLen, bFilterStops );
	if ( !uCRC || !m_bHitblock )
		return uCRC;

	return HitblockGetID ( (const char *)pWord, iLen, uCRC ); // !COMMIT would break, we kind of strcmp inside; but must never get called?
}

/// binary search for the first hit with wordid greater than or equal to reference
static CSphWordHit * FindFirstGte ( CSphWordHit * pHits, int iHits, SphWordID_t uID )
{
	if ( pHits->m_iWordID==uID )
		return pHits;

	CSphWordHit * pL = pHits;
	CSphWordHit * pR = pHits + iHits - 1;
	if ( pL->m_iWordID > uID || pR->m_iWordID < uID )
		return NULL;

	while ( pR-pL!=1 )
	{
		CSphWordHit * pM = pL + ( pR-pL )/2;
		if ( pM->m_iWordID < uID )
			pL = pM;
		else
			pR = pM;
	}

	assert ( pR-pL==1 );
	assert ( pL->m_iWordID<uID );
	assert ( pR->m_iWordID>=uID );
	return pR;
}

/// full crc and keyword check
static inline bool FullIsLess ( const CSphDictKeywords::HitblockException_t & a, const CSphDictKeywords::HitblockException_t & b )
{
	if ( a.m_uCRC!=b.m_uCRC )
		return a.m_uCRC < b.m_uCRC;
	return strcmp ( a.m_pEntry->m_pKeyword, b.m_pEntry->m_pKeyword ) < 0;
}

/// sort functor to compute collided hits reordering
struct HitblockPatchSort_fn
{
	const CSphDictKeywords::HitblockException_t * m_pExc;

	explicit HitblockPatchSort_fn ( const CSphDictKeywords::HitblockException_t * pExc )
		: m_pExc ( pExc )
	{}

	bool IsLess ( int a, int b ) const
	{
		return FullIsLess ( m_pExc[a], m_pExc[b] );
	}
};

/// do hit block patching magic
void CSphDictKeywords::HitblockPatch ( CSphWordHit * pHits, int iHits ) const
{
	if ( !pHits || iHits<=0 )
		return;

	const CSphVector<HitblockException_t> & dExc = m_dExceptions; // shortcut
	CSphVector<CSphWordHit*> dChunk;

	// reorder hit chunks for exceptions (aka crc collisions)
	for ( int iFirst = 0; iFirst < dExc.GetLength()-1; )
	{
		// find next span of collisions, iFirst inclusive, iMax exclusive ie. [iFirst,iMax)
		// (note that exceptions array is always sorted)
		SphWordID_t uFirstWordid = dExc[iFirst].m_pEntry->m_uWordid;
		assert ( dExc[iFirst].m_uCRC==uFirstWordid );

		int iMax = iFirst+1;
		SphWordID_t uSpan = uFirstWordid+1;
		while ( iMax < dExc.GetLength() && dExc[iMax].m_pEntry->m_uWordid==uSpan )
		{
			iMax++;
			uSpan++;
		}

		// check whether they are in proper order already
		bool bSorted = true;
		for ( int i=iFirst; i<iMax-1 && bSorted; i++ )
			if ( FullIsLess ( dExc[i+1], dExc[i] ) )
				bSorted = false;

		// order is ok; skip this span
		if ( bSorted )
		{
			iFirst = iMax;
			continue;
		}

		// we need to fix up these collision hits
		// convert them from arbitrary "wordid asc" to strict "crc asc, keyword asc" order
		// lets begin with looking up hit chunks for every wordid
		dChunk.Resize ( iMax-iFirst+1 );

		// find the end
		dChunk.Last() = FindFirstGte ( pHits, iHits, uFirstWordid+iMax-iFirst );
		if ( !dChunk.Last() )
		{
			assert ( iMax==dExc.GetLength() && pHits[iHits-1].m_iWordID==uFirstWordid+iMax-1-iFirst );
			dChunk.Last() = pHits+iHits;
		}

		// find the start
		dChunk[0] = FindFirstGte ( pHits, dChunk.Last()-pHits, uFirstWordid );
		assert ( dChunk[0] && dChunk[0]->m_iWordID==uFirstWordid );

		// find the chunk starts
		for ( int i=1; i<dChunk.GetLength()-1; i++ )
		{
			dChunk[i] = FindFirstGte ( dChunk[i-1], dChunk.Last()-dChunk[i-1], uFirstWordid+i );
			assert ( dChunk[i] && dChunk[i]->m_iWordID==uFirstWordid+i );
		}

		CSphWordHit * pTemp;
		if ( iMax-iFirst==2 )
		{
			// most frequent case, just two collisions
			// OPTIMIZE? allocate buffer for the smaller chunk, not just first chunk
			pTemp = new CSphWordHit [ dChunk[1]-dChunk[0] ];
			memcpy ( pTemp, dChunk[0], ( dChunk[1]-dChunk[0] )*sizeof(CSphWordHit) );
			memmove ( dChunk[0], dChunk[1], ( dChunk[2]-dChunk[1] )*sizeof(CSphWordHit) );
			memcpy ( dChunk[0] + ( dChunk[2]-dChunk[1] ), pTemp, ( dChunk[1]-dChunk[0] )*sizeof(CSphWordHit) );
		} else
		{
			// generic case, more than two
			CSphVector<int> dReorder ( iMax-iFirst );
			ARRAY_FOREACH ( i, dReorder )
				dReorder[i] = i;

			HitblockPatchSort_fn fnSort ( &dExc[iFirst] );
			dReorder.Sort ( fnSort );

			// OPTIMIZE? could skip heading and trailing blocks that are already in position
			pTemp = new CSphWordHit [ dChunk.Last()-dChunk[0] ];
			CSphWordHit * pOut = pTemp;

			ARRAY_FOREACH ( i, dReorder )
			{
				int iChunk = dReorder[i];
				int iChunkHits = dChunk[iChunk+1] - dChunk[iChunk];
				memcpy ( pOut, dChunk[iChunk], iChunkHits*sizeof(CSphWordHit) );
				pOut += iChunkHits;
			}

			assert ( ( pOut-pTemp )==( dChunk.Last()-dChunk[0] ) );
			memcpy ( dChunk[0], pTemp, ( dChunk.Last()-dChunk[0] )*sizeof(CSphWordHit) );
		}

		// patching done
		SafeDeleteArray ( pTemp );
		iFirst = iMax;
	}
}

const char * CSphDictKeywords::HitblockGetKeyword ( SphWordID_t uWordID )
{
	const DWORD uHash = (DWORD)( uWordID % SLOTS );

	HitblockKeyword_t * pEntry = m_dHash [ uHash ];
	while ( pEntry )
	{
		// check crc
		if ( pEntry->m_uWordid!=uWordID )
		{
			// crc mismatch, try next entry
			pEntry = pEntry->m_pNextHash;
			continue;
		}

		return pEntry->m_pKeyword;
	}

	assert ( m_dExceptions.GetLength() );
	ARRAY_FOREACH ( i, m_dExceptions )
		if ( m_dExceptions[i].m_pEntry->m_uWordid==uWordID )
			return m_dExceptions[i].m_pEntry->m_pKeyword;

	sphWarning ( "hash missing value in operator [] (wordid="INT64_FMT", hash=%d)", (int64_t)uWordID, uHash );
	assert ( 0 && "hash missing value in operator []" );
	return "\31oops";
}

//////////////////////////////////////////////////////////////////////////
// KEYWORDS STORING DICTIONARY
//////////////////////////////////////////////////////////////////////////

class CRtDictKeywords : public ISphRtDictWraper
{
private:
	CSphDict *				m_pBase;
	SmallStringHash_T<int>	m_hKeywords;
	CSphVector<BYTE>		m_dPackedKeywords;

	CSphString				m_sWarning;
	int						m_iKeywordsOverrun;

public:
	explicit CRtDictKeywords ( CSphDict * pBase )
		: m_pBase ( pBase )
		, m_iKeywordsOverrun ( 0 )
	{
		m_dPackedKeywords.Add ( 0 ); // avoid zero offset at all costs
	}
	virtual ~CRtDictKeywords() {}

	virtual SphWordID_t GetWordID ( BYTE * pWord )
	{
		SphWordID_t uCRC = m_pBase->GetWordID ( pWord );
		if ( uCRC )
			return AddKeyword ( pWord );
		else
			return 0;
	}

	virtual SphWordID_t GetWordIDWithMarkers ( BYTE * pWord )
	{
		SphWordID_t uCRC = m_pBase->GetWordIDWithMarkers ( pWord );
		if ( uCRC )
			return AddKeyword ( pWord );
		else
			return 0;
	}

	virtual SphWordID_t GetWordIDNonStemmed ( BYTE * pWord )
	{
		SphWordID_t uCRC = m_pBase->GetWordIDNonStemmed ( pWord );
		if ( uCRC )
			return AddKeyword ( pWord );
		else
			return 0;
	}

	virtual SphWordID_t GetWordID ( const BYTE * pWord, int iLen, bool bFilterStops )
	{
		SphWordID_t uCRC = m_pBase->GetWordID ( pWord, iLen, bFilterStops );
		if ( uCRC )
			return AddKeyword ( pWord );
		else
			return 0;
	}

	virtual const BYTE * GetPackedKeywords () { return m_dPackedKeywords.Begin(); }
	virtual int GetPackedLen () { return m_dPackedKeywords.GetLength(); }
	virtual void ResetKeywords()
	{
		m_dPackedKeywords.Resize ( 0 );
		m_dPackedKeywords.Add ( 0 ); // avoid zero offset at all costs
		m_hKeywords.Reset();
	}

	SphWordID_t AddKeyword ( const BYTE * pWord )
	{
		CSphString sWord;
		int iLen = strlen ( (const char *)pWord );
		// stemmer might squeeze out the word
		if ( !iLen )
			return 0;

		// fix of very long word (zones)
		if ( iLen>=( SPH_MAX_WORD_LEN*3 ) )
		{
			int iClippedLen = SPH_MAX_WORD_LEN*3;
			sWord.SetBinary ( (const char *)pWord, iClippedLen );
			if ( m_iKeywordsOverrun )
			{
				m_sWarning.SetSprintf ( "word overrun buffer, clipped!!! clipped='%s', length=%d(%d)", sWord.cstr(), iClippedLen, iLen );
			} else
			{
				m_sWarning.SetSprintf ( ", clipped='%s', length=%d(%d)", sWord.cstr(), iClippedLen, iLen );
			}
			iLen = iClippedLen;
			m_iKeywordsOverrun++;
		} else
		{
			sWord.SetBinary ( (const char *)pWord, iLen );
		}

		int * pOff = m_hKeywords ( sWord );
		if ( pOff )
		{
			return *pOff;
		}

		int iOff = m_dPackedKeywords.GetLength();
		m_dPackedKeywords.Resize ( iOff+iLen+1 );
		m_dPackedKeywords[iOff] = (BYTE)( iLen & 0xFF );
		memcpy ( m_dPackedKeywords.Begin()+iOff+1, pWord, iLen );

		m_hKeywords.Add ( iOff, sWord );

		return iOff;
	}

	virtual void LoadStopwords ( const char * sFiles, const ISphTokenizer * pTokenizer ) { m_pBase->LoadStopwords ( sFiles, pTokenizer ); }
	virtual void LoadStopwords ( const CSphVector<SphWordID_t> & dStopwords ) { m_pBase->LoadStopwords ( dStopwords ); }
	virtual void WriteStopwords ( CSphWriter & tWriter ) { m_pBase->WriteStopwords ( tWriter ); }
	virtual bool LoadWordforms ( const CSphVector<CSphString> & dFiles, const CSphEmbeddedFiles * pEmbedded, const ISphTokenizer * pTokenizer, const char * sIndex ) { return m_pBase->LoadWordforms ( dFiles, pEmbedded, pTokenizer, sIndex ); }
	virtual void WriteWordforms ( CSphWriter & tWriter ) { m_pBase->WriteWordforms ( tWriter ); }
	virtual int SetMorphology ( const char * szMorph, bool bUseUTF8, CSphString & sMessage ) { return m_pBase->SetMorphology ( szMorph, bUseUTF8, sMessage ); }
	virtual void Setup ( const CSphDictSettings & tSettings ) { m_pBase->Setup ( tSettings ); }
	virtual const CSphDictSettings & GetSettings () const { return m_pBase->GetSettings(); }
	virtual const CSphVector <CSphSavedFile> & GetStopwordsFileInfos () { return m_pBase->GetStopwordsFileInfos(); }
	virtual const CSphVector <CSphSavedFile> & GetWordformsFileInfos () { return m_pBase->GetWordformsFileInfos(); }
	virtual const CSphMultiformContainer * GetMultiWordforms () const { return m_pBase->GetMultiWordforms(); }
	virtual bool IsStopWord ( const BYTE * pWord ) const { return m_pBase->IsStopWord ( pWord ); }
	virtual const char * GetLastWarning() const { return m_iKeywordsOverrun ? m_sWarning.cstr() : NULL; }
	virtual void ResetWarning () { m_iKeywordsOverrun = 0; }
	virtual uint64_t GetSettingsFNV () const { return m_pBase->GetSettingsFNV(); }
	virtual void SetApplyMorph ( bool bApply ) { m_pBase->SetApplyMorph ( bApply ); }
};

ISphRtDictWraper * sphCreateRtKeywordsDictionaryWrapper ( CSphDict * pBase )
{
	return new CRtDictKeywords ( pBase );
}


//////////////////////////////////////////////////////////////////////////
// DICTIONARY FACTORIES
//////////////////////////////////////////////////////////////////////////

static CSphDict * SetupDictionary ( CSphDict * pDict, const CSphDictSettings & tSettings,
	const CSphEmbeddedFiles * pFiles, const ISphTokenizer * pTokenizer, const char * sIndex,
	CSphString & sError )
{
	assert ( pTokenizer );
	assert ( pDict );

	pDict->Setup ( tSettings );
	int iRet = pDict->SetMorphology ( tSettings.m_sMorphology.cstr (), pTokenizer->IsUtf8(), sError );
	if ( iRet==CSphDict::ST_ERROR )
	{
		SafeDelete ( pDict );
		return NULL;
	}

	if ( pFiles && pFiles->m_bEmbeddedStopwords )
		pDict->LoadStopwords ( pFiles->m_dStopwords );
	else
		pDict->LoadStopwords ( tSettings.m_sStopwords.cstr (), pTokenizer );

	pDict->LoadWordforms ( tSettings.m_dWordforms, pFiles && pFiles->m_bEmbeddedWordforms ? pFiles : NULL, pTokenizer, sIndex );

	return pDict;
}

CSphDict * sphCreateDictionaryTemplate ( const CSphDictSettings & tSettings,
									const CSphEmbeddedFiles * pFiles, const ISphTokenizer * pTokenizer, const char * sIndex,
									CSphString & sError )
{
	CSphDict * pDict = new CSphDictTemplate();
	if ( !pDict )
		return NULL;
	return SetupDictionary ( pDict, tSettings, pFiles, pTokenizer, sIndex, sError );
}


CSphDict * sphCreateDictionaryCRC ( const CSphDictSettings & tSettings,
	const CSphEmbeddedFiles * pFiles, const ISphTokenizer * pTokenizer, const char * sIndex,
	CSphString & sError )
{
	CSphDict * pDict = NULL;
	if ( tSettings.m_bCrc32 )
		pDict = new CSphDictCRC<true> ();
	else
		pDict = new CSphDictCRC<false> ();
	if ( !pDict )
		return NULL;
	return SetupDictionary ( pDict, tSettings, pFiles, pTokenizer, sIndex, sError );
}


CSphDict * sphCreateDictionaryKeywords ( const CSphDictSettings & tSettings,
	const CSphEmbeddedFiles * pFiles, const ISphTokenizer * pTokenizer, const char * sIndex,
	CSphString & sError )
{
	CSphDict * pDict = new CSphDictKeywords();
	return SetupDictionary ( pDict, tSettings, pFiles, pTokenizer, sIndex, sError );
}


void sphShutdownWordforms ()
{
	CSphVector<CSphSavedFile> dEmptyFiles;
	CSphDiskDictTraits::SweepWordformContainers ( dEmptyFiles );
}

/////////////////////////////////////////////////////////////////////////////
// HTML STRIPPER
/////////////////////////////////////////////////////////////////////////////

static inline int sphIsTag ( int c )
{
	return sphIsAlpha(c) || c=='.' || c==':';
}

static inline int sphIsTagStart ( int c )
{
	return ( c>='a' && c<='z' ) || ( c>='A' && c<='Z' ) || c=='_' || c=='.' || c==':';
}

CSphHTMLStripper::CSphHTMLStripper ( bool bDefaultTags )
{
	if ( bDefaultTags )
	{
		// known inline tags
		const char * dKnown[] =
		{
			"a", "b", "i", "s", "u",
			"basefont", "big", "em", "font", "img",
			"label", "small", "span", "strike", "strong",
			"sub\0", "sup\0", // fix gcc 3.4.3 on solaris10 compiler bug
			"tt"
		};

		m_dTags.Resize ( sizeof(dKnown)/sizeof(dKnown[0]) );
		ARRAY_FOREACH ( i, m_dTags )
		{
			m_dTags[i].m_sTag = dKnown[i];
			m_dTags[i].m_iTagLen = strlen ( dKnown[i] );
			m_dTags[i].m_bInline = true;
		}
	}

	UpdateTags ();
}


int CSphHTMLStripper::GetCharIndex ( int iCh ) const
{
	if ( iCh>='a' && iCh<='z' ) return iCh-'a';
	if ( iCh>='A' && iCh<='Z' ) return iCh-'A';
	if ( iCh=='_' ) return 26;
	if ( iCh==':' ) return 27;
	return -1;
}


void CSphHTMLStripper::UpdateTags ()
{
	m_dTags.Sort ();

	for ( int i=0; i<MAX_CHAR_INDEX; i++ )
	{
		m_dStart[i] = INT_MAX;
		m_dEnd[i] = -1;
	}

	ARRAY_FOREACH ( i, m_dTags )
	{
		int iIdx = GetCharIndex ( m_dTags[i].m_sTag.cstr()[0] );
		if ( iIdx<0 )
			continue;

		m_dStart[iIdx] = Min ( m_dStart[iIdx], i );
		m_dEnd[iIdx] = Max ( m_dEnd[iIdx], i );
	}
}


bool CSphHTMLStripper::SetIndexedAttrs ( const char * sConfig, CSphString & sError )
{
	if ( !sConfig || !*sConfig )
		return true;

	char sTag[256], sAttr[256];

	const char * p = sConfig, * s;
	#define LOC_ERROR(_msg,_pos) { sError.SetSprintf ( "SetIndexedAttrs(): %s near '%s'", _msg, _pos ); return false; }

	while ( *p )
	{
		// skip spaces
		while ( *p && isspace(*p) ) p++;
		if ( !*p ) break;

		// check tag name
		s = p; while ( sphIsTag(*p) ) p++;
		if ( s==p ) LOC_ERROR ( "invalid character in tag name", s );

		// get tag name
		if ( p-s>=(int)sizeof(sTag) ) LOC_ERROR ( "tag name too long", s );
		strncpy ( sTag, s, p-s );
		sTag[p-s] = '\0';

		// skip spaces
		while ( *p && isspace(*p) ) p++;
		if ( *p++!='=' ) LOC_ERROR ( "'=' expected", p-1 );

		// add indexed tag entry, if not there yet
		strlwr ( sTag );

		int iIndexTag = -1;
		ARRAY_FOREACH ( i, m_dTags )
			if ( m_dTags[i].m_sTag==sTag )
		{
			iIndexTag = i;
			break;
		}
		if ( iIndexTag<0 )
		{
			m_dTags.Add();
			m_dTags.Last().m_sTag = sTag;
			m_dTags.Last().m_iTagLen = strlen ( sTag );
			iIndexTag = m_dTags.GetLength()-1;
		}

		m_dTags[iIndexTag].m_bIndexAttrs = true;
		CSphVector<CSphString> & dAttrs = m_dTags[iIndexTag].m_dAttrs;

		// scan attributes
		while ( *p )
		{
			// skip spaces
			while ( *p && isspace(*p) ) p++;
			if ( !*p ) break;

			// check attr name
			s = p; while ( sphIsTag(*p) ) p++;
			if ( s==p ) LOC_ERROR ( "invalid character in attribute name", s );

			// get attr name
			if ( p-s>=(int)sizeof(sAttr) ) LOC_ERROR ( "attribute name too long", s );
			strncpy ( sAttr, s, p-s );
			sAttr[p-s] = '\0';

			// add attr, if not there yet
			int iAttr;
			for ( iAttr=0; iAttr<dAttrs.GetLength(); iAttr++ )
				if ( dAttrs[iAttr]==sAttr )
					break;

			if ( iAttr==dAttrs.GetLength() )
				dAttrs.Add ( sAttr );

			// skip spaces
			while ( *p && isspace(*p) ) p++;
			if ( !*p ) break;

			// check if there's next attr or tag
			if ( *p==',' ) { p++; continue; } // next attr
			if ( *p==';' ) { p++; break; } // next tag
			LOC_ERROR ( "',' or ';' or end of line expected", p );
		}
	}

	#undef LOC_ERROR

	UpdateTags ();
	return true;
}


bool CSphHTMLStripper::SetRemovedElements ( const char * sConfig, CSphString & )
{
	if ( !sConfig || !*sConfig )
		return true;

	const char * p = sConfig;
	while ( *p )
	{
		// skip separators
		while ( *p && !sphIsTag(*p) ) p++;
		if ( !*p ) break;

		// get tag name
		const char * s = p;
		while ( sphIsTag(*p) ) p++;

		CSphString sTag;
		sTag.SetBinary ( s, p-s );
		sTag.ToLower ();

		// mark it
		int iTag;
		for ( iTag=0; iTag<m_dTags.GetLength(); iTag++ )
			if ( m_dTags[iTag].m_sTag==sTag )
		{
			m_dTags[iTag].m_bRemove = true;
			break;
		}

		if ( iTag==m_dTags.GetLength() )
		{
			m_dTags.Add();
			m_dTags.Last().m_sTag = sTag;
			m_dTags.Last().m_iTagLen = strlen ( sTag.cstr() );
			m_dTags.Last().m_bRemove = true;
		}
	}

	UpdateTags ();
	return true;
}


void CSphHTMLStripper::EnableParagraphs ()
{
	// known block-level elements
	const char * dBlock[] = { "address", "blockquote", "caption", "center",
		"dd", "div", "dl", "dt", "h1", "h2", "h3", "h4", "h5", "li", "menu",
		"ol", "p", "pre", "table", "tbody", "td", "tfoot", "th", "thead",
		"tr", "ul", NULL };

	for ( int iBlock=0; dBlock[iBlock]; iBlock++ )
	{
		const char * sTag = dBlock[iBlock];

		// mark if known already
		int iTag;
		for ( iTag=0; iTag<m_dTags.GetLength(); iTag++ )
			if ( m_dTags[iTag].m_sTag==sTag )
		{
			m_dTags[iTag].m_bPara = true;
			break;
		}

		// add if not known yet
		if ( iTag==m_dTags.GetLength() )
		{
			StripperTag_t& dTag = m_dTags.Add();
			dTag.m_sTag = sTag;
			dTag.m_iTagLen = strlen(sTag);
			dTag.m_bPara = true;
		}
	}

	UpdateTags ();
}


bool CSphHTMLStripper::SetZones ( const char * sZones, CSphString & sError )
{
	// yet another mini parser!
	// index_zones = {tagname | prefix*} [, ...]
	if ( !sZones || !*sZones )
		return true;

	const char * s = sZones;
	while ( *s )
	{
		// skip spaces
		while ( sphIsSpace(*s) )
			s++;
		if ( !*s )
			break;

		// expect ident
		if ( !sphIsTagStart(*s) )
		{
			sError.SetSprintf ( "unexpected char near '%s' in index_zones", s );
			return false;
		}

		// get ident (either tagname or prefix*)
		const char * sTag = s;
		while ( sphIsTag(*s) )
			s++;

		const char * sTagEnd = s;
		bool bPrefix = false;
		if ( *s=='*' )
		{
			s++;
			bPrefix = true;
		}

		// skip spaces
		while ( sphIsSpace(*s) )
			s++;

		// expect eof or comma after ident
		if ( *s && *s!=',' )
		{
			sError.SetSprintf ( "unexpected char near '%s' in index_zones", s );
			return false;
		}
		if ( *s==',' )
			s++;

		// got valid entry, handle it
		CSphHTMLStripper::StripperTag_t & tTag = m_dTags.Add();
		tTag.m_sTag.SetBinary ( sTag, sTagEnd-sTag );
		tTag.m_iTagLen = (int)( sTagEnd-sTag );
		tTag.m_bZone = true;
		tTag.m_bZonePrefix = bPrefix;
	}

	UpdateTags ();
	return true;
}


const BYTE * SkipQuoted ( const BYTE * p )
{
	const BYTE * pMax = p + 512; // 512 bytes should be enough for a reasonable HTML attribute value, right?!
	const BYTE * pProbEnd = NULL; // (most) probable end location in case we don't find a matching quote
	BYTE cEnd = *p++; // either apostrophe or quote

	while ( p<pMax && *p && *p!=cEnd )
	{
		if ( !pProbEnd )
			if ( *p=='>' || *p=='\r' )
				pProbEnd = p;
		p++;
	}

	if ( *p==cEnd )
		return p+1;

	if ( pProbEnd )
		return pProbEnd;

	return p;
}


struct HtmlEntity_t
{
	const char *	m_sName;
	int				m_iCode;
};


static inline DWORD HtmlEntityHash ( const BYTE * str, int len )
{
	static const unsigned short asso_values[] =
	{
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 4,
		6, 22, 1, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 170, 48, 0, 5, 44,
		0, 10, 10, 86, 421, 7, 0, 1, 42, 93,
		41, 421, 0, 5, 8, 14, 421, 421, 5, 11,
		8, 421, 421, 421, 421, 421, 421, 1, 25, 27,
		9, 2, 113, 82, 14, 3, 179, 1, 81, 91,
		12, 0, 1, 180, 56, 17, 5, 31, 60, 7,
		3, 161, 2, 3, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421, 421, 421, 421,
		421, 421, 421, 421, 421, 421, 421
	};

	register int hval = len;
	switch ( hval )
	{
		default:	hval += asso_values [ str[4] ];
		case 4:
		case 3:		hval += asso_values [ str[2] ];
		case 2:		hval += asso_values [ str[1]+1 ];
		case 1:		hval += asso_values [ str[0] ];
					break;
	}
	return hval + asso_values [ str[len-1] ];
}


static inline int HtmlEntityLookup ( const BYTE * str, int len )
{
	static const unsigned char lengthtable[] =
	{
		0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 3, 3,
		4, 3, 3, 5, 3, 6, 5, 5, 3, 4, 4, 5, 3, 4,
		4, 0, 5, 4, 5, 6, 5, 6, 4, 5, 3, 3, 5, 0,
		0, 0, 0, 6, 0, 5, 5, 0, 5, 6, 6, 3, 0, 3,
		5, 3, 0, 6, 0, 4, 3, 6, 3, 6, 6, 6, 6, 5,
		5, 5, 5, 5, 5, 2, 6, 4, 0, 6, 3, 3, 3, 0,
		4, 5, 4, 4, 4, 3, 7, 4, 3, 6, 2, 3, 6, 4,
		3, 6, 5, 6, 5, 5, 4, 2, 0, 0, 4, 6, 8, 0,
		0, 0, 5, 5, 0, 6, 6, 2, 2, 4, 4, 6, 6, 4,
		4, 5, 6, 2, 3, 4, 6, 5, 0, 2, 0, 0, 6, 6,
		6, 6, 6, 4, 6, 5, 0, 6, 4, 5, 4, 6, 6, 0,
		0, 4, 6, 5, 6, 0, 6, 4, 5, 6, 5, 6, 4, 0,
		3, 6, 0, 4, 4, 4, 5, 4, 6, 0, 4, 4, 6, 5,
		6, 7, 2, 2, 6, 2, 5, 2, 5, 0, 0, 0, 4, 4,
		2, 4, 2, 2, 4, 0, 4, 4, 4, 5, 5, 0, 3, 7,
		5, 0, 5, 6, 5, 0, 6, 0, 6, 0, 4, 6, 4, 6,
		6, 2, 6, 0, 5, 5, 4, 6, 6, 0, 5, 6, 4, 4,
		4, 4, 0, 5, 0, 5, 0, 4, 5, 4, 0, 4, 4, 4,
		0, 0, 0, 4, 0, 0, 0, 5, 6, 5, 3, 0, 0, 6,
		5, 4, 5, 5, 5, 5, 0, 5, 5, 0, 5, 0, 0, 0,
		4, 6, 0, 3, 0, 5, 5, 0, 0, 3, 6, 5, 0, 4,
		0, 0, 0, 0, 5, 7, 5, 3, 5, 3, 0, 0, 6, 0,
		6, 0, 0, 7, 0, 0, 5, 0, 5, 0, 0, 0, 0, 5,
		4, 0, 0, 0, 0, 0, 7, 4, 0, 0, 3, 0, 0, 0,
		3, 0, 6, 0, 0, 7, 5, 5, 0, 3, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 5,
		5, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 4, 6, 0, 0, 0, 0, 0, 0, 0,
		4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		5
	};

	static const struct HtmlEntity_t wordlist[] =
	{
		{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"Rho", 929},
		{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"Chi", 935},
		{"phi", 966},
		{"iota", 953},
		{"psi", 968},
		{"int", 8747},
		{"theta", 952},
		{"amp", 38},
		{"there4", 8756},
		{"Theta", 920},
		{"omega", 969},
		{"and", 8743},
		{"prop", 8733},
		{"ensp", 8194},
		{"image", 8465},
		{"not", 172},
		{"isin", 8712},
		{"sdot", 8901},
		{"", 0},
		{"prime", 8242},
		{"prod", 8719},
		{"trade", 8482},
		{"Scaron", 352},
		{"kappa", 954},
		{"thinsp", 8201},
		{"emsp", 8195},
		{"thorn", 254},
		{"eta", 951},
		{"chi", 967},
		{"Kappa", 922},
		{"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"scaron", 353},
		{"", 0},
		{"notin", 8713},
		{"ndash", 8211},
		{"", 0},
		{"acute", 180},
		{"otilde", 245},
		{"atilde", 227},
		{"Phi", 934},
		{"", 0},
		{"Psi", 936},
		{"pound", 163},
		{"cap", 8745},
		{"", 0},
		{"otimes", 8855},
		{"", 0},
		{"nbsp", 32},
		{"rho", 961},
		{"ntilde", 241},
		{"eth", 240},
		{"oacute", 243},
		{"aacute", 225},
		{"eacute", 233},
		{"iacute", 237},
		{"nabla", 8711},
		{"Prime", 8243},
		{"ocirc", 244},
		{"acirc", 226},
		{"ecirc", 234},
		{"icirc", 238},
		{"or", 8744},
		{"Yacute", 221},
		{"nsub", 8836},
		{"", 0},
		{"Uacute", 218},
		{"Eta", 919},
		{"ETH", 208},
		{"sup", 8835},
		{"", 0},
		{"supe", 8839},
		{"Ucirc", 219},
		{"sup1", 185},
		{"para", 182},
		{"sup2", 178},
		{"loz", 9674},
		{"omicron", 959},
		{"part", 8706},
		{"cup", 8746},
		{"Ntilde", 209},
		{"Mu", 924},
		{"tau", 964},
		{"uacute", 250},
		{"Iota", 921},
		{"Tau", 932},
		{"rsaquo", 8250},
		{"alpha", 945},
		{"Ccedil", 199},
		{"ucirc", 251},
		{"oline", 8254},
		{"sup3", 179},
		{"nu", 957},
		{"", 0}, {"", 0},
		{"sube", 8838},
		{"Eacute", 201},
		{"thetasym", 977},
		{"", 0}, {"", 0}, {"", 0},
		{"Omega", 937},
		{"Ecirc", 202},
		{"", 0},
		{"lowast", 8727},
		{"iquest", 191},
		{"lt", 60},
		{"gt", 62},
		{"ordm", 186},
		{"euro", 8364},
		{"oslash", 248},
		{"lsaquo", 8249},
		{"zeta", 950},
		{"cong", 8773},
		{"mdash", 8212},
		{"ccedil", 231},
		{"ne", 8800},
		{"sub", 8834},
		{"Zeta", 918},
		{"Lambda", 923},
		{"Gamma", 915},
		{"", 0},
		{"Nu", 925},
		{"", 0}, {"", 0},
		{"ograve", 242},
		{"agrave", 224},
		{"egrave", 232},
		{"igrave", 236},
		{"frac14", 188},
		{"ordf", 170},
		{"Otilde", 213},
		{"infin", 8734},
		{"", 0},
		{"frac12", 189},
		{"beta", 946},
		{"radic", 8730},
		{"darr", 8595},
		{"Iacute", 205},
		{"Ugrave", 217},
		{"", 0}, {"", 0},
		{"harr", 8596},
		{"hearts", 9829},
		{"Icirc", 206},
		{"Oacute", 211},
		{"", 0},
		{"frac34", 190},
		{"cent", 162},
		{"crarr", 8629},
		{"curren", 164},
		{"Ocirc", 212},
		{"brvbar", 166},
		{"sect", 167},
		{"", 0},
		{"ang", 8736},
		{"ugrave", 249},
		{"", 0},
		{"Beta", 914},
		{"uarr", 8593},
		{"dArr", 8659},
		{"asymp", 8776},
		{"perp", 8869},
		{"Dagger", 8225},
		{"", 0},
		{"hArr", 8660},
		{"rang", 9002},
		{"dagger", 8224},
		{"exist", 8707},
		{"Egrave", 200},
		{"Omicron", 927},
		{"mu", 956},
		{"pi", 960},
		{"weierp", 8472},
		{"xi", 958},
		{"clubs", 9827},
		{"Xi", 926},
		{"aring", 229},
		{"", 0}, {"", 0}, {"", 0},
		{"copy", 169},
		{"uArr", 8657},
		{"ni", 8715},
		{"rarr", 8594},
		{"le", 8804},
		{"ge", 8805},
		{"zwnj", 8204},
		{"", 0},
		{"apos", 39},
		{"macr", 175},
		{"lang", 9001},
		{"gamma", 947},
		{"Delta", 916},
		{"", 0},
		{"uml", 168},
		{"alefsym", 8501},
		{"delta", 948},
		{"", 0},
		{"bdquo", 8222},
		{"lambda", 955},
		{"equiv", 8801},
		{"", 0},
		{"Oslash", 216},
		{"", 0},
		{"hellip", 8230},
		{"", 0},
		{"rArr", 8658},
		{"Atilde", 195},
		{"larr", 8592},
		{"spades", 9824},
		{"Igrave", 204},
		{"Pi", 928},
		{"yacute", 253},
		{"", 0},
		{"diams", 9830},
		{"sbquo", 8218},
		{"fnof", 402},
		{"Ograve", 210},
		{"plusmn", 177},
		{"", 0},
		{"rceil", 8969},
		{"Aacute", 193},
		{"ouml", 246},
		{"auml", 228},
		{"euml", 235},
		{"iuml", 239},
		{"", 0},
		{"Acirc", 194},
		{"", 0},
		{"rdquo", 8221},
		{"", 0},
		{"lArr", 8656},
		{"rsquo", 8217},
		{"Yuml", 376},
		{"", 0},
		{"quot", 34},
		{"Uuml", 220},
		{"bull", 8226},
		{"", 0}, {"", 0}, {"", 0},
		{"real", 8476},
		{"", 0}, {"", 0}, {"", 0},
		{"lceil", 8968},
		{"permil", 8240},
		{"upsih", 978},
		{"sum", 8721},
		{"", 0}, {"", 0},
		{"divide", 247},
		{"raquo", 187},
		{"uuml", 252},
		{"ldquo", 8220},
		{"Alpha", 913},
		{"szlig", 223},
		{"lsquo", 8216},
		{"", 0},
		{"Sigma", 931},
		{"tilde", 732},
		{"", 0},
		{"THORN", 222},
		{"", 0}, {"", 0}, {"", 0},
		{"Euml", 203},
		{"rfloor", 8971},
		{"", 0},
		{"lrm", 8206},
		{"", 0},
		{"sigma", 963},
		{"iexcl", 161},
		{"", 0}, {"", 0},
		{"deg", 176},
		{"middot", 183},
		{"laquo", 171},
		{"", 0},
		{"circ", 710},
		{"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"frasl", 8260},
		{"epsilon", 949},
		{"oplus", 8853},
		{"yen", 165},
		{"micro", 181},
		{"piv", 982},
		{"", 0}, {"", 0},
		{"lfloor", 8970},
		{"", 0},
		{"Agrave", 192},
		{"", 0}, {"", 0},
		{"Upsilon", 933},
		{"", 0}, {"", 0},
		{"times", 215},
		{"", 0},
		{"cedil", 184},
		{"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"minus", 8722},
		{"Iuml", 207},
		{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"upsilon", 965},
		{"Ouml", 214},
		{"", 0}, {"", 0},
		{"rlm", 8207},
		{"", 0}, {"", 0}, {"", 0},
		{"reg", 174},
		{"", 0},
		{"forall", 8704},
		{"", 0}, {"", 0},
		{"Epsilon", 917},
		{"empty", 8709},
		{"OElig", 338},
		{"", 0},
		{"shy", 173},
		{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"Aring", 197},
		{"", 0}, {"", 0}, {"", 0},
		{"oelig", 339},
		{"aelig", 230},
		{"", 0},
		{"zwj", 8205},
		{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"sim", 8764},
		{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"yuml", 255},
		{"sigmaf", 962},
		{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"Auml", 196},
		{"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"", 0}, {"", 0}, {"", 0}, {"", 0},
		{"AElig", 198}
	};

	const int MIN_WORD_LENGTH		= 2;
	const int MAX_WORD_LENGTH		= 8;
	const int MAX_HASH_VALUE		= 420;

	if ( len<=MAX_WORD_LENGTH && len>=MIN_WORD_LENGTH )
	{
		register int key = HtmlEntityHash ( str, len );
		if ( key<=MAX_HASH_VALUE && key>=0 )
			if ( len==lengthtable[key] )
		{
			register const char * s = wordlist[key].m_sName;
			if ( *str==*s && !memcmp ( str+1, s+1, len-1 ) )
				return wordlist[key].m_iCode;
		}
	}
	return 0;
}


void CSphHTMLStripper::Strip ( BYTE * sData ) const
{
	const BYTE * s = sData;
	BYTE * d = sData;
	for ( ;; )
	{
		/////////////////////////////////////
		// scan until eof, or tag, or entity
		/////////////////////////////////////

		while ( *s && *s!='<' && *s!='&' )
		{
			if ( *s>=0x20 )
				*d++ = *s;
			else
				*d++ = ' ';
			s++;
		}
		if ( !*s )
			break;

		/////////////////
		// handle entity
		/////////////////

		if ( *s=='&' )
		{
			if ( s[1]=='#' )
			{
				// handle "&#number;" form
				int iCode = 0;
				s += 2;
				while ( isdigit(*s) )
					iCode = iCode*10 + (*s++) - '0';

				if ( ( iCode>=0 && iCode<=0x1f ) || *s!=';' ) // 0-31 are reserved codes
					continue;

				d += sphUTF8Encode ( d, iCode );
				s++;

			} else
			{
				// skip until ';' or max length
				if ( ( s[1]>='a' && s[1]<='z' ) || ( s[1]>='A' && s[1]<='Z' ) )
				{
					const int MAX_ENTITY_LEN = 8;
					const BYTE * sStart = s+1;
					while ( *s && *s!=';' && s-sStart<=MAX_ENTITY_LEN )
						s++;

					if ( *s==';' )
					{
						int iCode = HtmlEntityLookup ( sStart, (int)(s-sStart) );
						if ( iCode>0 )
						{
							// this is a known entity; encode it
							d += sphUTF8Encode ( d, iCode );
							s++;
							continue;
						}
					}

					// rollback
					s = sStart-1;
				}

				// if we're here, it's not an entity; pass the leading ampersand and rescan
				*d++ = *s++;
			}
			continue;
		}

		//////////////
		// handle tag
		//////////////

		assert ( *s=='<' );
		if ( GetCharIndex(s[1])<0 )
		{
			if ( s[1]=='/' )
			{
				// check if it's valid closing tag
				if ( GetCharIndex(s[2])<0 )
				{
					*d++ = *s++;
					continue;
				}

			} else if ( s[1]=='!' )
			{
				if ( s[2]=='-' && s[3]=='-' )
				{
					// it's valid comment; scan until comment end
					s += 4; // skip opening '<!--'
					while ( *s )
					{
						if ( s[0]=='-' && s[1]=='-' && s[2]=='>' )
							break;
						s++;
					}
					if ( !*s )
						break;
					s += 3; // skip closing '-->'
					continue;

				} else if ( isalpha(s[2]) )
				{
					// it's <!doctype> style PI; scan until PI end
					s += 2;
					while ( *s && *s!='>' )
					{
						if ( *s=='\'' || *s=='"' )
						{
							s = SkipQuoted ( s );
							while ( isspace(*s) ) s++;
						} else
						{
							s++;
						}
					}
					if ( *s=='>' )
						s++;
					continue;

				} else
				{
					// it's something malformed; just ignore
					*d++ = *s++;
					continue;
				}

			} else if ( s[1]=='?' )
			{
				// scan until PI end
				s += 2; // skip opening '<?'
				while ( *s )
				{
					if ( s[0]=='?' && s[1]=='>' )
						break;
					s++;
				}
				if ( !*s )
					break;
				s += 2; // skip closing '?>'
				continue;

			} else
			{
				// simply malformed
				*d++ = *s++;
				continue;
			}
		}
		s++; // skip '<'

		//////////////////////////////////////
		// lookup this tag in known tags list
		//////////////////////////////////////

		const StripperTag_t * pTag = NULL;
		int iZoneNameLen = 0;
		const BYTE * sZoneName = NULL;
		s = FindTag ( s, &pTag, &sZoneName, &iZoneNameLen );

		/////////////////////////////////////
		// process tag contents
		// index attributes if needed
		// gracefully handle malformed stuff
		/////////////////////////////////////

#define LOC_SKIP_SPACES() { while ( sphIsSpace(*s) ) s++; if ( !*s || *s=='>' ) break; }

		bool bIndexAttrs = ( pTag && pTag->m_bIndexAttrs );
		while ( *s && *s!='>' )
		{
			LOC_SKIP_SPACES();
			if ( sphIsTagStart(*s) )
			{
				// skip attribute name while it's valid
				const BYTE * sAttr = s;
				while ( sphIsTag(*s) )
					s++;

				// blanks or a value after a valid attribute name?
				if ( sphIsSpace(*s) || *s=='=' )
				{
					const int iAttrLen = (int)( s - sAttr );
					LOC_SKIP_SPACES();

					// a valid name but w/o a value; keep scanning
					if ( *s!='=' )
						continue;

					// got value!
					s++;
					LOC_SKIP_SPACES();

					// check attribute name
					// OPTIMIZE! remove linear search
					int iAttr = -1;
					if ( bIndexAttrs )
					{
						for ( iAttr=0; iAttr<pTag->m_dAttrs.GetLength(); iAttr++ )
						{
							int iLen = strlen ( pTag->m_dAttrs[iAttr].cstr() );
							if ( iLen==iAttrLen && !strncasecmp ( pTag->m_dAttrs[iAttr].cstr(), (const char*)sAttr, iLen ) )
								break;
						}
						if ( iAttr==pTag->m_dAttrs.GetLength() )
							iAttr = -1;
					}

					// process the value
					const BYTE * sVal = s;
					if ( *s=='\'' || *s=='"' )
					{
						// skip quoted value until a matching quote
						s = SkipQuoted ( s );
					} else
					{
						// skip unquoted value until tag end or whitespace
						while ( *s && *s!='>' && !sphIsSpace(*s) )
							s++;
					}

					// if this one is to be indexed, copy it
					if ( iAttr>=0 )
					{
						const BYTE * sMax = s;
						if ( *sVal=='\'' || *sVal=='"' )
						{
							if ( sMax[-1]==sVal[0] )
								sMax--;
							sVal++;
						}
						while ( sVal<sMax )
							*d++ = *sVal++;
						*d++ = ' ';
					}

					// handled the value; keep scanning
					continue;
				}

				// nope, got an invalid character in the sequence (or maybe eof)
				// fall through to an invalid name handler
			}

			// keep skipping until tag end or whitespace
			while ( *s && *s!='>' && !sphIsSpace(*s) )
				s++;
		}

#undef LOC_SKIP_SPACES

		// skip closing angle bracket, if any
		if ( *s )
			s++;

		// unknown tag is done; others might require a bit more work
		if ( !pTag )
		{
			*d++ = ' '; // unknown tags are *not* inline by default
			continue;
		}

		// handle zones
		if ( pTag->m_bZone )
		{
			// should be at tag's end
			assert ( s[0]=='\0' || s[-1]=='>' );

			// emit secret codes
			*d++ = MAGIC_CODE_ZONE;
			for ( int i=0; i<iZoneNameLen; i++ )
				*d++ = (BYTE) tolower ( sZoneName[i] );
			if ( *d )
				*d++ = MAGIC_CODE_ZONE;

			if ( !*s )
				break;
			continue;
		}

		// handle paragraph boundaries
		if ( pTag->m_bPara )
		{
			*d++ = MAGIC_CODE_PARAGRAPH;
			continue;
		}

		// in all cases, the tag must be fully processed at this point
		// not a remove-tag? we're done
		if ( !pTag->m_bRemove )
		{
			if ( !pTag->m_bInline )
				*d++ = ' ';
			continue;
		}

		// sudden eof? bail out
		if ( !*s )
			break;

		// must be a proper remove-tag end, then
		assert ( pTag->m_bRemove && s[-1]=='>' );

		// short-form? we're done
		if ( s[-2]=='/' )
			continue;

		// skip everything until the closing tag
		// FIXME! should we handle insane cases with quoted closing tag within tag?
		for ( ;; )
		{
			while ( *s && ( s[0]!='<' || s[1]!='/' ) ) s++;
			if ( !*s ) break;

			s += 2; // skip </
			if ( strncasecmp ( pTag->m_sTag.cstr(), (const char*)s, pTag->m_iTagLen )!=0 ) continue;
			if ( !sphIsTag ( s[pTag->m_iTagLen] ) )
			{
				s += pTag->m_iTagLen; // skip tag
				if ( *s=='>' ) s++;
				break;
			}
		}

		if ( !pTag->m_bInline ) *d++ = ' ';
	}
	*d++ = '\0';

	// space, paragraph sequences elimination pass
	s = sData;
	d = sData;
	bool bSpaceOut = false;
	bool bParaOut = false;
	bool bZoneOut = false;
	while ( const char c = *s++ )
	{
		assert ( d<=s-1 );

		// handle different character classes
		if ( sphIsSpace(c) )
		{
			// handle whitespace, skip dupes
			if ( !bSpaceOut )
				*d++ = ' ';

			bSpaceOut = true;
			continue;

		} else if ( c==MAGIC_CODE_PARAGRAPH )
		{
			// handle paragraph marker, skip dupes
			if ( !bParaOut && !bZoneOut )
			{
				*d++ = c;
				bParaOut = true;
			}

			bSpaceOut = true;
			continue;

		} else if ( c==MAGIC_CODE_ZONE )
		{
			// zone marker
			// rewind preceding paragraph, if any, it is redundant
			if ( bParaOut )
			{
				assert ( d>sData && d[-1]==MAGIC_CODE_PARAGRAPH );
				d--;
			}

			// copy \4zoneid\4
			*d++ = c;
			while ( *s && *s!=MAGIC_CODE_ZONE )
				*d++ = *s++;

			if ( *s )
				*d++ = *s++;

			// update state
			// no spaces paragraphs allowed
			bSpaceOut = bZoneOut = true;
			bParaOut = false;
			continue;

		} else
		{
			*d++ = c;
			bSpaceOut = bParaOut = bZoneOut = false;
		}
	}
	*d++ = '\0';
}

const BYTE * CSphHTMLStripper::FindTag ( const BYTE * sSrc, const StripperTag_t ** ppTag,
	const BYTE ** ppZoneName, int * pZoneNameLen ) const
{
	assert ( sSrc && ppTag && ppZoneName && pZoneNameLen );
	assert ( sSrc[0]!='/' || sSrc[1]!='\0' );

	const BYTE * sTagName = ( sSrc[0]=='/' ) ? sSrc+1 : sSrc;

	*ppZoneName = sSrc;
	*pZoneNameLen = 0;

	int iIdx = GetCharIndex ( sTagName[0] );
	assert ( iIdx>=0 && iIdx<MAX_CHAR_INDEX );

	if ( m_dEnd[iIdx]>=0 )
	{
		int iStart = m_dStart[iIdx];
		int iEnd = m_dEnd[iIdx];

		for ( int i=iStart; i<=iEnd; i++ )
		{
			int iLen = m_dTags[i].m_iTagLen;
			int iCmp = strncasecmp ( m_dTags[i].m_sTag.cstr(), (const char*)sTagName, iLen );

			// the tags are sorted; so if current candidate is already greater, rest can be skipped
			if ( iCmp>0 )
				break;

			// do we have a match?
			if ( iCmp==0 )
			{
				// got exact match?
				if ( !sphIsTag ( sTagName[iLen] ) )
				{
					*ppTag = m_dTags.Begin() + i;
					sSrc = sTagName + iLen; // skip tag name
					if ( m_dTags[i].m_bZone )
						*pZoneNameLen = sSrc - *ppZoneName;
					break;
				}

				// got wildcard match?
				if ( m_dTags[i].m_bZonePrefix )
				{
					*ppTag = m_dTags.Begin() + i;
					sSrc = sTagName + iLen;
					while ( sphIsTag(*sSrc) )
						sSrc++;
					*pZoneNameLen = sSrc - *ppZoneName;
					break;
				}
			}
		}
	}

	return sSrc;
}

bool CSphHTMLStripper::IsValidTagStart ( int iCh ) const
{
	int i = GetCharIndex ( iCh );
	return ( i>=0 && i<MAX_CHAR_INDEX );
}

//////////////////////////////////////////////////////////////////////////
#if USE_RE2
class CSphFieldRegExps : public ISphFieldFilter
{
public:
	explicit				CSphFieldRegExps ( bool bUTF8 );
	virtual					~CSphFieldRegExps ();

	virtual	int				Apply ( const BYTE * sField, int iLength, CSphVector<BYTE> & dStorage );
	virtual	void			GetSettings ( CSphFieldFilterSettings & tSettings ) const;

	bool					AddRegExp ( const char * sRegExp, CSphString & sError );

private:
	struct RegExp_t
	{
		CSphString	m_sFrom;
		CSphString	m_sTo;

		RE2 *		m_pRE2;
	};

	CSphVector<RegExp_t>	m_dRegexps;
	bool					m_bUTF8;
};


CSphFieldRegExps::CSphFieldRegExps ( bool bUTF8 )
	: m_bUTF8 ( bUTF8 )
{
}

CSphFieldRegExps::~CSphFieldRegExps ()
{
	ARRAY_FOREACH ( i, m_dRegexps )
		SafeDelete ( m_dRegexps[i].m_pRE2 );
}

int CSphFieldRegExps::Apply ( const BYTE * sField, int iLength, CSphVector<BYTE> & dStorage )
{
	dStorage.Resize ( 0 );
	if ( !sField || !*sField )
		return 0;

	bool bReplaced = false;
	std::string sRe2 = ( iLength ? std::string ( (char *) sField, iLength ) : (char *) sField );
	ARRAY_FOREACH ( i, m_dRegexps )
	{
		assert ( m_dRegexps[i].m_pRE2 );
		bReplaced |= ( RE2::GlobalReplace ( &sRe2, *m_dRegexps[i].m_pRE2, m_dRegexps[i].m_sTo.cstr() )>0 );
	}

	if ( !bReplaced )
		return 0;

	int iDstLen = sRe2.length();
	dStorage.Resize ( iDstLen+4 ); // string SAFETY_GAP
	strncpy ( (char *)dStorage.Begin(), sRe2.c_str(), dStorage.GetLength() );
	return iDstLen;
}

void CSphFieldRegExps::GetSettings ( CSphFieldFilterSettings & tSettings ) const
{
	tSettings.m_bUTF8 = m_bUTF8;
	tSettings.m_dRegexps.Resize ( m_dRegexps.GetLength() );
	ARRAY_FOREACH ( i, m_dRegexps )
		tSettings.m_dRegexps[i].SetSprintf ( "%s => %s", m_dRegexps[i].m_sFrom.cstr(), m_dRegexps[i].m_sTo.cstr() );
}

bool CSphFieldRegExps::AddRegExp ( const char * sRegExp, CSphString & sError )
{
	const char sSplitter [] = "=>";
	const char * sSplit = strstr ( sRegExp, sSplitter );
	if ( !sSplit )
	{
		sError = "mapping token (=>) not found";
		return false;
	} else if ( strstr ( sSplit + strlen ( sSplitter ), sSplitter ) )
	{
		sError = "mapping token (=>) found more than once";
		return false;
	}

	m_dRegexps.Resize ( m_dRegexps.GetLength () + 1 );
	RegExp_t & tRegExp = m_dRegexps.Last();
	tRegExp.m_sFrom.SetBinary ( sRegExp, sSplit-sRegExp );
	tRegExp.m_sTo = sSplit + strlen ( sSplitter );
	tRegExp.m_sFrom.Trim();
	tRegExp.m_sTo.Trim();

	RE2::Options tOptions;
	tOptions.set_utf8 ( m_bUTF8 );
	tRegExp.m_pRE2 = new RE2 ( tRegExp.m_sFrom.cstr(), tOptions );

	std::string sRE2Error;
	if ( !tRegExp.m_pRE2->CheckRewriteString ( tRegExp.m_sTo.cstr(), &sRE2Error ) )
	{
		sError.SetSprintf ( "\"%s => %s\" is not a valid mapping: %s", tRegExp.m_sFrom.cstr(), tRegExp.m_sTo.cstr(), sRE2Error.c_str() );
		SafeDelete ( tRegExp.m_pRE2 );
		m_dRegexps.Remove ( m_dRegexps.GetLength() - 1 );
		return false;
	}

	return true;
}
#endif

#if USE_RE2
ISphFieldFilter * sphCreateFieldFilter ( const CSphFieldFilterSettings & tFilterSettings, CSphString & sError )
{
	CSphFieldRegExps * pFilter = new CSphFieldRegExps ( tFilterSettings.m_bUTF8 );
	ARRAY_FOREACH ( i, tFilterSettings.m_dRegexps )
		pFilter->AddRegExp ( tFilterSettings.m_dRegexps[i].cstr(), sError );

	return pFilter;
}
#else
ISphFieldFilter * sphCreateFieldFilter ( const CSphFieldFilterSettings &, CSphString & )
{
	return NULL;
}
#endif


/////////////////////////////////////////////////////////////////////////////
// GENERIC SOURCE
/////////////////////////////////////////////////////////////////////////////

CSphSourceSettings::CSphSourceSettings ()
	: m_iMinPrefixLen ( 0 )
	, m_iMinInfixLen ( 0 )
	, m_iMaxSubstringLen ( 0 )
	, m_iBoundaryStep ( 0 )
	, m_bIndexExactWords ( false )
	, m_iOvershortStep ( 1 )
	, m_iStopwordStep ( 1 )
	, m_bIndexSP ( false )
	, m_bIndexFieldLens ( false )
{}


ESphWordpart CSphSourceSettings::GetWordpart ( const char * sField, bool bWordDict )
{
	if ( bWordDict )
		return SPH_WORDPART_WHOLE;

	bool bPrefix = ( m_iMinPrefixLen>0 ) && ( m_dPrefixFields.GetLength()==0 || m_dPrefixFields.Contains ( sField ) );
	bool bInfix = ( m_iMinInfixLen>0 ) && ( m_dInfixFields.GetLength()==0 || m_dInfixFields.Contains ( sField ) );

	assert ( !( bPrefix && bInfix ) ); // no field must be marked both prefix and infix
	if ( bPrefix )
		return SPH_WORDPART_PREFIX;
	if ( bInfix )
		return SPH_WORDPART_INFIX;
	return SPH_WORDPART_WHOLE;
}

//////////////////////////////////////////////////////////////////////////

CSphSource::CSphSource ( const char * sName )
	: m_pTokenizer ( NULL )
	, m_pDict ( NULL )
	, m_pFieldFilter ( NULL )
	, m_tSchema ( sName )
	, m_bStripHTML ( false )
	, m_iNullIds ( 0 )
	, m_iMaxIds ( 0 )
{
	m_pStripper = new CSphHTMLStripper ( true );
}


CSphSource::~CSphSource()
{
	delete m_pStripper;
}


void CSphSource::SetDict ( CSphDict * pDict )
{
	assert ( pDict );
	m_pDict = pDict;
}


const CSphSourceStats & CSphSource::GetStats ()
{
	return m_tStats;
}


bool CSphSource::SetStripHTML ( const char * sExtractAttrs, const char * sRemoveElements,
	bool bDetectParagraphs, const char * sZones, CSphString & sError )
{
	if ( !m_pStripper->SetIndexedAttrs ( sExtractAttrs, sError ) )
		return false;

	if ( !m_pStripper->SetRemovedElements ( sRemoveElements, sError ) )
		return false;

	if ( bDetectParagraphs )
		m_pStripper->EnableParagraphs ();

	if ( !m_pStripper->SetZones ( sZones, sError ) )
		return false;

	m_bStripHTML = true;
	return true;
}


void CSphSource::SetFieldFilter ( ISphFieldFilter * pFilter )
{
	m_pFieldFilter = pFilter;
}

void CSphSource::SetTokenizer ( ISphTokenizer * pTokenizer )
{
	assert ( pTokenizer );
	m_pTokenizer = pTokenizer;
}


bool CSphSource::UpdateSchema ( CSphSchema * pInfo, CSphString & sError )
{
	assert ( pInfo );

	// fill it
	if ( pInfo->m_dFields.GetLength()==0 && pInfo->GetAttrsCount()==0 )
	{
		*pInfo = m_tSchema;
		return true;
	}

	// check it
	return m_tSchema.CompareTo ( *pInfo, sError );
}


void CSphSource::Setup ( const CSphSourceSettings & tSettings )
{
	m_iMinPrefixLen = Max ( tSettings.m_iMinPrefixLen, 0 );
	m_iMinInfixLen = Max ( tSettings.m_iMinInfixLen, 0 );
	m_iMaxSubstringLen = Max ( tSettings.m_iMaxSubstringLen, 0 );
	m_iBoundaryStep = Max ( tSettings.m_iBoundaryStep, -1 );
	m_bIndexExactWords = tSettings.m_bIndexExactWords;
	m_iOvershortStep = Min ( Max ( tSettings.m_iOvershortStep, 0 ), 1 );
	m_iStopwordStep = Min ( Max ( tSettings.m_iStopwordStep, 0 ), 1 );
	m_bIndexSP = tSettings.m_bIndexSP;
	m_dPrefixFields = tSettings.m_dPrefixFields;
	m_dInfixFields = tSettings.m_dInfixFields;
	m_bIndexFieldLens = tSettings.m_bIndexFieldLens;
}


SphDocID_t CSphSource::VerifyID ( SphDocID_t uID )
{
	if ( uID==0 )
	{
		m_iNullIds++;
		return 0;
	}

	if ( uID==DOCID_MAX )
	{
		m_iMaxIds++;
		return 0;
	}

	return uID;
}


ISphHits * CSphSource::IterateJoinedHits ( CSphString & )
{
	static ISphHits dDummy;
	m_tDocInfo.m_iDocID = 0; // pretend that's an eof
	return &dDummy;
}

/////////////////////////////////////////////////////////////////////////////
// DOCUMENT SOURCE
/////////////////////////////////////////////////////////////////////////////

static void FormatEscaped ( FILE * fp, const char * sLine )
{
	// handle empty lines
	if ( !sLine || !*sLine )
	{
		fprintf ( fp, "''" );
		return;
	}

	// pass one, count the needed buffer size
	int iLen = strlen(sLine);
	int iOut = 0;
	for ( int i=0; i<iLen; i++ )
		switch ( sLine[i] )
	{
		case '\t':
		case '\'':
		case '\\':
			iOut += 2;
			break;

		default:
			iOut++;
			break;
	}
	iOut += 2; // quotes

	// allocate the buffer
	char sMinibuffer[8192];
	char * sMaxibuffer = NULL;
	char * sBuffer = sMinibuffer;

	if ( iOut>(int)sizeof(sMinibuffer) )
	{
		sMaxibuffer = new char [ iOut+4 ]; // 4 is just my safety gap
		sBuffer = sMaxibuffer;
	}

	// pass two, escape it
	char * sOut = sBuffer;
	*sOut++ = '\'';

	for ( int i=0; i<iLen; i++ )
		switch ( sLine[i] )
	{
		case '\t':
		case '\'':
		case '\\':	*sOut++ = '\\'; // no break intended
		default:	*sOut++ = sLine[i];
	}
	*sOut++ = '\'';

	// print!
	assert ( sOut==sBuffer+iOut );
	fwrite ( sBuffer, 1, iOut, fp );

	// cleanup
	SafeDeleteArray ( sMaxibuffer );
}

CSphSource_Document::CSphBuildHitsState_t::CSphBuildHitsState_t ()
	: m_bProcessingHits ( false )
	, m_bDocumentDone ( false )
	, m_dFields ( NULL )
	, m_iStartPos ( 0 )
	, m_iHitPos ( 0 )
	, m_iField ( 0 )
	, m_iStartField ( 0 )
	, m_iEndField ( 0 )
	, m_iBuildLastStep ( 1 )
{
}

CSphSource_Document::CSphBuildHitsState_t::~CSphBuildHitsState_t ()
{
	ARRAY_FOREACH ( i, m_dTmpFieldStorage )
		SafeDeleteArray ( m_dTmpFieldStorage[i] );
}

CSphSource_Document::CSphSource_Document ( const char * sName )
	: CSphSource ( sName )
	, m_pReadFileBuffer ( NULL )
	, m_iReadFileBufferSize ( 256 * 1024 )
	, m_iMaxFileBufferSize ( 2 * 1024 * 1024 )
	, m_eOnFileFieldError ( FFE_IGNORE_FIELD )
	, m_fpDumpRows ( NULL )
	, m_iPlainFieldsLength ( 0 )
	, m_pFieldLengthAttrs ( NULL )
	, m_bIdsSorted ( false )
	, m_iMaxHits ( MAX_SOURCE_HITS )
{
}


bool CSphSource_Document::IterateDocument ( CSphString & sError )
{
	assert ( m_pTokenizer );
	assert ( !m_tState.m_bProcessingHits );

	m_tHits.m_dData.Resize ( 0 );

	m_tState = CSphBuildHitsState_t();
	m_tState.m_iEndField = m_iPlainFieldsLength;
	m_tState.m_dTmpFieldPtrs.Resize ( m_tState.m_iEndField );
	m_tState.m_dTmpFieldStorage.Resize ( m_tState.m_iEndField );

	ARRAY_FOREACH ( i, m_tState.m_dTmpFieldPtrs )
	{
		m_tState.m_dTmpFieldPtrs[i] = NULL;
		m_tState.m_dTmpFieldStorage[i] = NULL;
	}

	m_dMva.Resize ( 1 ); // must not have zero offset

	// fetch next document
	for ( ;; )
	{
		m_tState.m_dFields = NextDocument ( sError );
		if ( HasJoinedFields() )
			m_dAllIds.Add ( m_tDocInfo.m_iDocID );
		if ( m_tDocInfo.m_iDocID==0 )
			return true;

		if ( !m_tState.m_dFields )
			return false;

		// tricky bit
		// we can only skip document indexing from here, IterateHits() is too late
		// so in case the user chose to skip documents with file field problems
		// we need to check for those here
		if ( m_eOnFileFieldError==FFE_SKIP_DOCUMENT || m_eOnFileFieldError==FFE_FAIL_INDEX )
		{
			bool bOk = true;
			for ( int iField=0; iField<m_tState.m_iEndField && bOk; iField++ )
			{
				const BYTE * sFilename = m_tState.m_dFields[iField];
				if ( m_tSchema.m_dFields[iField].m_bFilename )
					bOk &= CheckFileField ( sFilename );

				if ( !bOk && m_eOnFileFieldError==FFE_FAIL_INDEX )
				{
					sError.SetSprintf ( "error reading file field data (docid=" DOCID_FMT ", filename=%s)",
						m_tDocInfo.m_iDocID, sFilename );
					return false;
				}
			}
			if ( !bOk && m_eOnFileFieldError==FFE_SKIP_DOCUMENT )
				continue;
		}

		if ( m_pFieldFilter )
		{
			// new field strings may be longer than original, that's why we need temporary storage
			ARRAY_FOREACH ( i, m_tState.m_dTmpFieldStorage )
				SafeDeleteArray ( m_tState.m_dTmpFieldStorage[i] );

			bool bHaveModifiedFields = false;
			for ( int iField=0; iField<m_tState.m_iEndField; iField++ )
			{
				if ( m_tSchema.m_dFields[iField].m_bFilename )
				{
					m_tState.m_dTmpFieldPtrs[iField] = m_tState.m_dFields[iField];
					continue;
				}

				CSphVector<BYTE> dFiltered;
				if ( m_pFieldFilter->Apply ( m_tState.m_dFields[iField], 0, dFiltered ) )
				{
					m_tState.m_dTmpFieldStorage[iField] = dFiltered.LeakData();
					m_tState.m_dTmpFieldPtrs[iField] = m_tState.m_dTmpFieldStorage[iField];
					bHaveModifiedFields = true;
				} else
				{
					m_tState.m_dTmpFieldPtrs[iField] = m_tState.m_dFields[iField];
				}
			}

			if ( bHaveModifiedFields )
				m_tState.m_dFields = (BYTE **)&( m_tState.m_dTmpFieldPtrs[0] );
		}

		// we're good
		break;
	}

	m_tStats.m_iTotalDocuments++;
	return true;
}


ISphHits * CSphSource_Document::IterateHits ( CSphString & sError )
{
	if ( m_tState.m_bDocumentDone )
		return NULL;

	m_tHits.m_dData.Resize ( 0 );

	BuildHits ( sError, false );

	return &m_tHits;
}


bool CSphSource_Document::CheckFileField ( const BYTE * sField )
{
	CSphAutofile tFileSource;
	CSphString sError;

	if ( tFileSource.Open ( (const char *)sField, SPH_O_READ, sError )==-1 )
	{
		sphWarning ( "docid=" DOCID_FMT ": %s", m_tDocInfo.m_iDocID, sError.cstr() );
		return false;
	}

	int64_t iFileSize = tFileSource.GetSize();
	if ( iFileSize+16 > m_iMaxFileBufferSize )
	{
		sphWarning ( "docid=" DOCID_FMT ": file '%s' too big for a field (size="INT64_FMT", max_file_field_buffer=%d)",
			m_tDocInfo.m_iDocID, (const char *)sField, iFileSize, m_iMaxFileBufferSize );
		return false;
	}

	return true;
}


/// returns file size on success, and replaces *ppField with a pointer to data
/// returns -1 on failure (and emits a warning)
int CSphSource_Document::LoadFileField ( BYTE ** ppField, CSphString & sError )
{
	CSphAutofile tFileSource;

	BYTE * sField = *ppField;
	if ( tFileSource.Open ( (const char *)sField, SPH_O_READ, sError )==-1 )
	{
		sphWarning ( "docid=" DOCID_FMT ": %s", m_tDocInfo.m_iDocID, sError.cstr() );
		return -1;
	}

	int64_t iFileSize = tFileSource.GetSize();
	if ( iFileSize+16 > m_iMaxFileBufferSize )
	{
		sphWarning ( "docid=" DOCID_FMT ": file '%s' too big for a field (size="INT64_FMT", max_file_field_buffer=%d)",
			m_tDocInfo.m_iDocID, (const char *)sField, iFileSize, m_iMaxFileBufferSize );
		return -1;
	}

	int iFieldBytes = (int)iFileSize;
	if ( !iFieldBytes )
		return 0;

	int iBufSize = Max ( m_iReadFileBufferSize, 1 << sphLog2 ( iFieldBytes+15 ) );
	if ( m_iReadFileBufferSize < iBufSize )
		SafeDeleteArray ( m_pReadFileBuffer );

	if ( !m_pReadFileBuffer )
	{
		m_pReadFileBuffer = new char [ iBufSize ];
		m_iReadFileBufferSize = iBufSize;
	}

	if ( !tFileSource.Read ( m_pReadFileBuffer, iFieldBytes, sError ) )
	{
		sphWarning ( "docid=" DOCID_FMT ": read failed: %s", m_tDocInfo.m_iDocID, sError.cstr() );
		return -1;
	}

	m_pReadFileBuffer[iFieldBytes] = '\0';

	*ppField = (BYTE*)m_pReadFileBuffer;
	return iFieldBytes;
}


bool AddFieldLens ( CSphSchema & tSchema, bool bDynamic, CSphString & sError )
{
	ARRAY_FOREACH ( i, tSchema.m_dFields )
	{
		CSphColumnInfo tCol;
		tCol.m_sName.SetSprintf ( "%s_len", tSchema.m_dFields[i].m_sName.cstr() );

		int iGot = tSchema.GetAttrIndex ( tCol.m_sName.cstr() );
		if ( iGot>=0 )
		{
			if ( tSchema.GetAttr(iGot).m_eAttrType==SPH_ATTR_TOKENCOUNT )
			{
				// looks like we already added these
				assert ( tSchema.GetAttr(iGot).m_sName==tCol.m_sName );
				return true;
			}

			sError.SetSprintf ( "attribute %s conflicts with index_field_lengths=1; remove it", tCol.m_sName.cstr() );
			return false;
		}

		tCol.m_eAttrType = SPH_ATTR_TOKENCOUNT;
		tSchema.AddAttr ( tCol, bDynamic ); // everything's dynamic at indexing time
	}
	return true;
}


bool CSphSource_Document::AddAutoAttrs ( CSphString & sError )
{
	// auto-computed length attributes
	if ( m_bIndexFieldLens )
		return AddFieldLens ( m_tSchema, true, sError );
	return true;
}


void CSphSource_Document::AllocDocinfo()
{
	// tricky bit
	// with in-config schema, attr storage gets allocated in Setup() when source is initially created
	// so when this AddAutoAttrs() additionally changes the count, we have to change the number of attributes
	// but Reset() prohibits that, because that is usually a programming mistake, hence the Swap() dance
	CSphMatch tNew;
	tNew.Reset ( m_tSchema.GetRowSize() );
	Swap ( m_tDocInfo, tNew );

	m_dStrAttrs.Resize ( m_tSchema.GetAttrsCount() );

	if ( m_bIndexFieldLens && m_tSchema.GetAttrsCount() )
	{
		int iFirst = m_tSchema.GetAttrsCount() - m_tSchema.m_dFields.GetLength();
		assert ( m_tSchema.GetAttr ( iFirst ).m_eAttrType==SPH_ATTR_TOKENCOUNT );
		assert ( m_tSchema.GetAttr ( iFirst+m_tSchema.m_dFields.GetLength()-1 ).m_eAttrType==SPH_ATTR_TOKENCOUNT );

		m_pFieldLengthAttrs = m_tDocInfo.m_pDynamic + ( m_tSchema.GetAttr ( iFirst ).m_tLocator.m_iBitOffset / 32 );
	}
}

//////////////////////////////////////////////////////////////////////////
// HIT GENERATORS
//////////////////////////////////////////////////////////////////////////

bool CSphSource_Document::BuildZoneHits ( SphDocID_t uDocid, BYTE * sWord )
{
	if ( *sWord==MAGIC_CODE_SENTENCE || *sWord==MAGIC_CODE_PARAGRAPH || *sWord==MAGIC_CODE_ZONE )
	{
		m_tHits.AddHit ( uDocid, m_pDict->GetWordID ( (BYTE*)MAGIC_WORD_SENTENCE ), m_tState.m_iHitPos );

		if ( *sWord==MAGIC_CODE_PARAGRAPH || *sWord==MAGIC_CODE_ZONE )
			m_tHits.AddHit ( uDocid, m_pDict->GetWordID ( (BYTE*)MAGIC_WORD_PARAGRAPH ), m_tState.m_iHitPos );

		if ( *sWord==MAGIC_CODE_ZONE )
		{
			BYTE * pZone = (BYTE*) m_pTokenizer->GetBufferPtr();
			BYTE * pEnd = pZone;
			while ( *pEnd && *pEnd!=MAGIC_CODE_ZONE )
			{
				pEnd++;
			}

			if ( *pEnd && *pEnd==MAGIC_CODE_ZONE )
			{
				*pEnd = '\0';
				m_tHits.AddHit ( uDocid, m_pDict->GetWordID ( pZone-1 ), m_tState.m_iHitPos );
				m_pTokenizer->SetBufferPtr ( (const char*) pEnd+1 );
			}
		}

		m_tState.m_iBuildLastStep = 1;
		return true;
	}
	return false;
}


// track blended start and reset on not blended token
static int TrackBlendedStart ( const ISphTokenizer * pTokenizer, int iBlendedHitsStart, int iHitsCount )
{
	iBlendedHitsStart = ( ( pTokenizer->TokenIsBlended() || pTokenizer->TokenIsBlendedPart() ) ? iBlendedHitsStart : -1 );
	if ( pTokenizer->TokenIsBlended() )
		iBlendedHitsStart = iHitsCount;

	return iBlendedHitsStart;
}


#define BUILD_SUBSTRING_HITS_COUNT 4

void CSphSource_Document::BuildSubstringHits ( SphDocID_t uDocid, bool bPayload, ESphWordpart eWordpart, bool bSkipEndMarker )
{
	bool bPrefixField = ( eWordpart==SPH_WORDPART_PREFIX );
	bool bInfixMode = m_iMinInfixLen > 0;

	int iMinInfixLen = bPrefixField ? m_iMinPrefixLen : m_iMinInfixLen;
	if ( !m_tState.m_bProcessingHits )
		m_tState.m_iBuildLastStep = 1;

	BYTE * sWord = NULL;
	BYTE sBuf [ 16+3*SPH_MAX_WORD_LEN ];

	int iIterHitCount = BUILD_SUBSTRING_HITS_COUNT;
	if ( bPrefixField )
		iIterHitCount += SPH_MAX_WORD_LEN - m_iMinPrefixLen;
	else
		iIterHitCount += ( ( m_iMinInfixLen+SPH_MAX_WORD_LEN ) * ( SPH_MAX_WORD_LEN-m_iMinInfixLen ) / 2 );

	// FIELDEND_MASK at blended token stream should be set for HEAD token too
	int iBlendedHitsStart = -1;

	// index all infixes
	while ( ( m_iMaxHits==0 || m_tHits.m_dData.GetLength()+iIterHitCount<m_iMaxHits )
		&& ( sWord = m_pTokenizer->GetToken() )!=NULL )
	{
		int iLastBlendedStart = TrackBlendedStart ( m_pTokenizer, iBlendedHitsStart, m_tHits.Length() );

		if ( !bPayload )
		{
			HITMAN::AddPos ( &m_tState.m_iHitPos, m_tState.m_iBuildLastStep + m_pTokenizer->GetOvershortCount()*m_iOvershortStep );
			if ( m_pTokenizer->GetBoundary() )
				HITMAN::AddPos ( &m_tState.m_iHitPos, m_iBoundaryStep );
			m_tState.m_iBuildLastStep = 1;
		}

		if ( BuildZoneHits ( uDocid, sWord ) )
			continue;

		int iLen = m_pTokenizer->GetLastTokenLen ();

		// always index full word (with magic head/tail marker(s))
		int iBytes = strlen ( (const char*)sWord );
		memcpy ( sBuf + 1, sWord, iBytes );
		sBuf[iBytes+1] = '\0';

		if ( m_bIndexExactWords )
		{
			sBuf[0] = MAGIC_WORD_HEAD_NONSTEMMED;
			m_tHits.AddHit ( uDocid, m_pDict->GetWordIDNonStemmed ( sBuf ), m_tState.m_iHitPos );
		}

		sBuf[0] = MAGIC_WORD_HEAD;

		// stemmed word w/markers
		SphWordID_t iWord = m_pDict->GetWordIDWithMarkers ( sBuf );
		if ( !iWord )
		{
			m_tState.m_iBuildLastStep = m_iStopwordStep;
			continue;
		}
		iBlendedHitsStart = iLastBlendedStart;
		m_tHits.AddHit ( uDocid, iWord, m_tState.m_iHitPos );
		m_tState.m_iBuildLastStep = m_pTokenizer->TokenIsBlended() ? 0 : 1;

		// restore stemmed word
		int iStemmedLen = strlen ( ( const char *)sBuf );
		sBuf [iStemmedLen - 1] = '\0';

		// stemmed word w/o markers
		if ( strcmp ( (const char *)sBuf + 1, (const char *)sWord ) )
			m_tHits.AddHit ( uDocid, m_pDict->GetWordID ( sBuf + 1, iStemmedLen - 2, true ), m_tState.m_iHitPos );

		// restore word
		memcpy ( sBuf + 1, sWord, iBytes );
		sBuf[iBytes+1] = MAGIC_WORD_TAIL;
		sBuf[iBytes+2] = '\0';

		// if there are no infixes, that's it
		if ( iMinInfixLen > iLen )
		{
			// index full word
			m_tHits.AddHit ( uDocid, m_pDict->GetWordID ( sWord ), m_tState.m_iHitPos );
			continue;
		}

		// process all infixes
		int iMaxStart = bPrefixField ? 0 : ( iLen - iMinInfixLen );

		BYTE * sInfix = sBuf + 1;

		for ( int iStart=0; iStart<=iMaxStart; iStart++ )
		{
			BYTE * sInfixEnd = sInfix;
			for ( int i = 0; i < iMinInfixLen; i++ )
				sInfixEnd += m_pTokenizer->GetCodepointLength ( *sInfixEnd );

			int iMaxSubLen = ( iLen-iStart );
			if ( m_iMaxSubstringLen )
				iMaxSubLen = Min ( m_iMaxSubstringLen, iMaxSubLen );

			for ( int i=iMinInfixLen; i<=iMaxSubLen; i++ )
			{
				m_tHits.AddHit ( uDocid, m_pDict->GetWordID ( sInfix, sInfixEnd-sInfix, false ), m_tState.m_iHitPos );

				// word start: add magic head
				if ( bInfixMode && iStart==0 )
					m_tHits.AddHit ( uDocid, m_pDict->GetWordID ( sInfix - 1, sInfixEnd-sInfix + 1, false ), m_tState.m_iHitPos );

				// word end: add magic tail
				if ( bInfixMode && i==iLen-iStart )
					m_tHits.AddHit ( uDocid, m_pDict->GetWordID ( sInfix, sInfixEnd-sInfix+1, false ), m_tState.m_iHitPos );

				sInfixEnd += m_pTokenizer->GetCodepointLength ( *sInfixEnd );
			}

			sInfix += m_pTokenizer->GetCodepointLength ( *sInfix );
		}
	}

	m_tState.m_bProcessingHits = ( sWord!=NULL );

	// mark trailing hits
	// and compute fields lengths
	if ( !bSkipEndMarker && !m_tState.m_bProcessingHits && m_tHits.Length() )
	{
		CSphWordHit * pHit = const_cast < CSphWordHit * > ( m_tHits.Last() );
		Hitpos_t uRefPos = pHit->m_iWordPos;

		if ( m_pFieldLengthAttrs )
			m_pFieldLengthAttrs [ HITMAN::GetField ( pHit->m_iWordPos ) ] = HITMAN::GetPos ( pHit->m_iWordPos );

		for ( ; pHit>=m_tHits.First() && pHit->m_iWordPos==uRefPos; pHit-- )
			HITMAN::SetEndMarker ( &pHit->m_iWordPos );

		// mark blended HEAD as trailing too
		if ( iBlendedHitsStart>=0 )
		{
			assert ( iBlendedHitsStart>=0 && iBlendedHitsStart<m_tHits.Length() );
			pHit = const_cast < CSphWordHit * > ( m_tHits.First()+iBlendedHitsStart );
			uRefPos = pHit->m_iWordPos;

			const CSphWordHit * pEnd = m_tHits.First()+m_tHits.Length();
			for ( ; pHit<pEnd && pHit->m_iWordPos==uRefPos; pHit++ )
				HITMAN::SetEndMarker ( &pHit->m_iWordPos );
		}
	}
}


#define BUILD_REGULAR_HITS_COUNT 6

void CSphSource_Document::BuildRegularHits ( SphDocID_t uDocid, bool bPayload, bool bSkipEndMarker )
{
	bool bWordDict = m_pDict->GetSettings().m_bWordDict;
	bool bGlobalPartialMatch = !bWordDict && ( m_iMinPrefixLen > 0 || m_iMinInfixLen > 0 );

	if ( !m_tState.m_bProcessingHits )
		m_tState.m_iBuildLastStep = 1;

	BYTE * sWord = NULL;
	BYTE sBuf [ 16+3*SPH_MAX_WORD_LEN ];

	// FIELDEND_MASK at blended token stream should be set for HEAD token too
	int iBlendedHitsStart = -1;

	// index words only
	while ( ( m_iMaxHits==0 || m_tHits.m_dData.GetLength()+BUILD_REGULAR_HITS_COUNT<m_iMaxHits )
		&& ( sWord = m_pTokenizer->GetToken() )!=NULL )
	{
		m_pDict->SetApplyMorph ( m_pTokenizer->GetMorphFlag() );

		int iLastBlendedStart = TrackBlendedStart ( m_pTokenizer, iBlendedHitsStart, m_tHits.Length() );


		if ( !bPayload )
		{
			HITMAN::AddPos ( &m_tState.m_iHitPos, m_tState.m_iBuildLastStep + m_pTokenizer->GetOvershortCount()*m_iOvershortStep );
			if ( m_pTokenizer->GetBoundary() )
				HITMAN::AddPos ( &m_tState.m_iHitPos, m_iBoundaryStep );
		}

		if ( BuildZoneHits ( uDocid, sWord ) )
			continue;

		if ( bGlobalPartialMatch )
		{
			int iBytes = strlen ( (const char*)sWord );
			memcpy ( sBuf + 1, sWord, iBytes );
			sBuf[0] = MAGIC_WORD_HEAD;
			sBuf[iBytes+1] = '\0';
			m_tHits.AddHit ( uDocid, m_pDict->GetWordIDWithMarkers ( sBuf ), m_tState.m_iHitPos );
		}

		ESphTokenMorph eMorph = m_pTokenizer->GetTokenMorph();
		if ( m_bIndexExactWords && eMorph!=SPH_TOKEN_MORPH_GUESS )
		{
			int iBytes = strlen ( (const char*)sWord );
			memcpy ( sBuf + 1, sWord, iBytes );
			sBuf[0] = MAGIC_WORD_HEAD_NONSTEMMED;
			sBuf[iBytes+1] = '\0';
			m_tHits.AddHit ( uDocid, m_pDict->GetWordIDNonStemmed ( sBuf ), m_tState.m_iHitPos );
		}

		if ( m_bIndexExactWords && eMorph==SPH_TOKEN_MORPH_ORIGINAL )
		{
			m_tState.m_iBuildLastStep = m_pTokenizer->TokenIsBlended() ? 0 : 1;
			continue;
		}

		SphWordID_t iWord = m_pDict->GetWordID ( sWord );
		if ( iWord )
		{
#if 0
			if ( HITMAN::GetPos ( m_tState.m_iHitPos )==1 )
				printf ( "\n" );
			printf ( "doc %d. pos %d. %s\n", uDocid, HITMAN::GetPos ( m_tState.m_iHitPos ), sWord );
#endif
			iBlendedHitsStart = iLastBlendedStart;
			m_tHits.AddHit ( uDocid, iWord, m_tState.m_iHitPos );
			m_tState.m_iBuildLastStep = m_pTokenizer->TokenIsBlended() ? 0 : 1;
		} else
			m_tState.m_iBuildLastStep = m_iStopwordStep;
	}

	m_tState.m_bProcessingHits = ( sWord!=NULL );

	// mark trailing hit
	// and compute field lengths
	if ( !bSkipEndMarker && !m_tState.m_bProcessingHits && m_tHits.Length() )
	{
		CSphWordHit * pHit = const_cast < CSphWordHit * > ( m_tHits.Last() );
		HITMAN::SetEndMarker ( &pHit->m_iWordPos );

		if ( m_pFieldLengthAttrs )
			m_pFieldLengthAttrs [ HITMAN::GetField ( pHit->m_iWordPos ) ] = HITMAN::GetPos ( pHit->m_iWordPos );

		// mark blended HEAD as trailing too
		if ( iBlendedHitsStart>=0 )
		{
			assert ( iBlendedHitsStart>=0 && iBlendedHitsStart<m_tHits.Length() );
			CSphWordHit * pBlendedHit = const_cast < CSphWordHit * > ( m_tHits.First() + iBlendedHitsStart );
			HITMAN::SetEndMarker ( &pBlendedHit->m_iWordPos );
		}
	}
}


void CSphSource_Document::BuildHits ( CSphString & sError, bool bSkipEndMarker )
{
	SphDocID_t uDocid = m_tDocInfo.m_iDocID;

	CSphVector<BYTE> dFiltered;
	for ( ; m_tState.m_iField<m_tState.m_iEndField; m_tState.m_iField++ )
	{
		if ( !m_tState.m_bProcessingHits )
		{
			// get that field
			BYTE * sField = m_tState.m_dFields[m_tState.m_iField-m_tState.m_iStartField];
			if ( !sField || !(*sField) )
				continue;

			// load files
			int iFieldBytes;
			const BYTE * sTextToIndex;
			if ( m_tSchema.m_dFields[m_tState.m_iField].m_bFilename )
			{
				LoadFileField ( &sField, sError );
				sTextToIndex = sField;
				iFieldBytes = (int) strlen ( (char*)sField );
				if ( m_pFieldFilter && iFieldBytes )
				{
					int iFiltered = m_pFieldFilter->Apply ( sTextToIndex, iFieldBytes, dFiltered );
					if ( iFiltered )
					{
						sTextToIndex = dFiltered.Begin();
						iFieldBytes = iFiltered;
					}
				}
			} else
			{
				iFieldBytes = (int) strlen ( (char*)sField );
				sTextToIndex = sField;
			}

			if ( iFieldBytes<=0 )
				continue;

			// strip html
			if ( m_bStripHTML )
			{
				m_pStripper->Strip ( (BYTE*)sTextToIndex );
				iFieldBytes = (int) strlen ( (char*)sTextToIndex );
			}

			// tokenize and build hits
			m_tStats.m_iTotalBytes += iFieldBytes;

			m_pTokenizer->SetBuffer ( (BYTE*)sTextToIndex, iFieldBytes );

			m_tState.m_iHitPos = HITMAN::Create ( m_tState.m_iField, m_tState.m_iStartPos );
		}

		const CSphColumnInfo & tField = m_tSchema.m_dFields[m_tState.m_iField];

		if ( tField.m_eWordpart!=SPH_WORDPART_WHOLE )
			BuildSubstringHits ( uDocid, tField.m_bPayload, tField.m_eWordpart, bSkipEndMarker );
		else
			BuildRegularHits ( uDocid, tField.m_bPayload, bSkipEndMarker );

		m_pDict->SetApplyMorph ( true );

		if ( m_tState.m_bProcessingHits )
			break;
	}

	m_tState.m_bDocumentDone = !m_tState.m_bProcessingHits;
}

//////////////////////////////////////////////////////////////////////////

SphRange_t CSphSource_Document::IterateFieldMVAStart ( int iAttr )
{
	SphRange_t tRange;
	tRange.m_iStart = tRange.m_iLength = 0;

	if ( iAttr<0 || iAttr>=m_tSchema.GetAttrsCount() )
		return tRange;

	const CSphColumnInfo & tMva = m_tSchema.GetAttr ( iAttr );
	int uOff = MVA_DOWNSIZE ( m_tDocInfo.GetAttr ( tMva.m_tLocator ) );
	if ( !uOff )
		return tRange;

	int iCount = m_dMva[uOff];
	assert ( iCount );

	tRange.m_iStart = uOff+1;
	tRange.m_iLength = iCount;

	return tRange;
}


static int sphAddMva64 ( CSphVector<DWORD> & dStorage, int64_t iVal )
{
	int uOff = dStorage.GetLength();
	dStorage.Resize ( uOff+2 );
	dStorage[uOff] = MVA_DOWNSIZE ( iVal );
	dStorage[uOff+1] = MVA_DOWNSIZE ( ( iVal>>32 ) & 0xffffffff );
	return uOff;
}


int CSphSource_Document::ParseFieldMVA ( CSphVector < DWORD > & dMva, const char * szValue, bool bMva64 ) const
{
	if ( !szValue )
		return 0;

	const char * pPtr = szValue;
	const char * pDigit = NULL;
	const int MAX_NUMBER_LEN = 64;
	char szBuf [MAX_NUMBER_LEN];

	assert ( dMva.GetLength() ); // must not have zero offset
	int uOff = dMva.GetLength();
	dMva.Add ( 0 ); // reserve value for count

	while ( *pPtr )
	{
		if ( ( *pPtr>='0' && *pPtr<='9' ) || ( bMva64 && *pPtr=='-' ) )
		{
			if ( !pDigit )
				pDigit = pPtr;
		} else
		{
			if ( pDigit )
			{
				if ( pPtr - pDigit < MAX_NUMBER_LEN )
				{
					strncpy ( szBuf, pDigit, pPtr - pDigit );
					szBuf [pPtr - pDigit] = '\0';
					if ( !bMva64 )
						dMva.Add ( sphToDword ( szBuf ) );
					else
						sphAddMva64 ( dMva, sphToInt64 ( szBuf ) );
				}

				pDigit = NULL;
			}
		}

		pPtr++;
	}

	if ( pDigit )
	{
		if ( !bMva64 )
			dMva.Add ( sphToDword ( pDigit ) );
		else
			sphAddMva64 ( dMva, sphToInt64 ( pDigit ) );
	}

	int iCount = dMva.GetLength()-uOff-1;
	if ( !iCount )
	{
		dMva.Pop(); // remove reserved value for count in case of 0 MVAs
		return 0;
	} else
	{
		dMva[uOff] = iCount;
		return uOff; // return offset to ( count, [value] )
	}
}

/////////////////////////////////////////////////////////////////////////////
// GENERIC SQL SOURCE
/////////////////////////////////////////////////////////////////////////////

CSphSourceParams_SQL::CSphSourceParams_SQL ()
	: m_iRangeStep ( 1024 )
	, m_iRefRangeStep ( 1024 )
	, m_bPrintQueries ( false )
	, m_iRangedThrottle ( 0 )
	, m_iMaxFileBufferSize ( 0 )
	, m_eOnFileFieldError ( FFE_IGNORE_FIELD )
	, m_iPort ( 0 )
{
}


const char * const CSphSource_SQL::MACRO_VALUES [ CSphSource_SQL::MACRO_COUNT ] =
{
	"$start",
	"$end"
};


CSphSource_SQL::CSphSource_SQL ( const char * sName )
	: CSphSource_Document	( sName )
	, m_bSqlConnected		( false )
	, m_uMinID				( 0 )
	, m_uMaxID				( 0 )
	, m_uCurrentID			( 0 )
	, m_uMaxFetchedID		( 0 )
	, m_iMultiAttr			( -1 )
	, m_iSqlFields			( 0 )
	, m_bCanUnpack			( false )
	, m_bUnpackFailed		( false )
	, m_bUnpackOverflow		( false )
	, m_iJoinedHitField		( -1 )
	, m_iJoinedHitID		( 0 )
	, m_iJoinedHitPos		( 0 )
{
}


bool CSphSource_SQL::Setup ( const CSphSourceParams_SQL & tParams )
{
	// checks
	assert ( !tParams.m_sQuery.IsEmpty() );

	m_tParams = tParams;

	// defaults
	#define LOC_FIX_NULL(_arg) if ( !m_tParams._arg.cstr() ) m_tParams._arg = "";
	LOC_FIX_NULL ( m_sHost );
	LOC_FIX_NULL ( m_sUser );
	LOC_FIX_NULL ( m_sPass );
	LOC_FIX_NULL ( m_sDB );
	#undef LOC_FIX_NULL

	#define LOC_FIX_QARRAY(_arg) \
		ARRAY_FOREACH ( i, m_tParams._arg ) \
			if ( m_tParams._arg[i].IsEmpty() ) \
				m_tParams._arg.Remove ( i-- );
	LOC_FIX_QARRAY ( m_dQueryPre );
	LOC_FIX_QARRAY ( m_dQueryPost );
	LOC_FIX_QARRAY ( m_dQueryPostIndex );
	#undef LOC_FIX_QARRAY

	// build and store default DSN for error reporting
	char sBuf [ 1024 ];
	snprintf ( sBuf, sizeof(sBuf), "sql://%s:***@%s:%d/%s",
		m_tParams.m_sUser.cstr(), m_tParams.m_sHost.cstr(),
		m_tParams.m_iPort, m_tParams.m_sDB.cstr() );
	m_sSqlDSN = sBuf;

	if ( m_tParams.m_iMaxFileBufferSize > 0 )
		m_iMaxFileBufferSize = m_tParams.m_iMaxFileBufferSize;
	m_eOnFileFieldError = m_tParams.m_eOnFileFieldError;

	return true;
}

const char * SubstituteParams ( const char * sQuery, const char * const * dMacroses, const char ** dValues, int iMcount )
{
	// OPTIMIZE? things can be precalculated
	const char * sCur = sQuery;
	int iLen = 0;
	while ( *sCur )
	{
		if ( *sCur=='$' )
		{
			int i;
			for ( i=0; i<iMcount; i++ )
				if ( strncmp ( dMacroses[i], sCur, strlen ( dMacroses[i] ) )==0 )
				{
					sCur += strlen ( dMacroses[i] );
					iLen += strlen ( dValues[i] );
					break;
				}
				if ( i<iMcount )
					continue;
		}

		sCur++;
		iLen++;
	}
	iLen++; // trailing zero

	// do interpolation
	char * sRes = new char [ iLen ];
	sCur = sQuery;

	char * sDst = sRes;
	while ( *sCur )
	{
		if ( *sCur=='$' )
		{
			int i;
			for ( i=0; i<iMcount; i++ )
				if ( strncmp ( dMacroses[i], sCur, strlen ( dMacroses[i] ) )==0 )
				{
					strcpy ( sDst, dValues[i] ); // NOLINT
					sCur += strlen ( dMacroses[i] );
					sDst += strlen ( dValues[i] );
					break;
				}
				if ( i<iMcount )
					continue;
		}
		*sDst++ = *sCur++;
	}
	*sDst++ = '\0';
	assert ( sDst-sRes==iLen );
	return sRes;
}


bool CSphSource_SQL::RunQueryStep ( const char * sQuery, CSphString & sError )
{
	sError = "";

	if ( m_tParams.m_iRangeStep<=0 )
		return false;
	if ( m_uCurrentID>m_uMaxID )
		return false;

	static const int iBufSize = 32;
	const char * sRes = NULL;

	sphSleepMsec ( m_tParams.m_iRangedThrottle );

	//////////////////////////////////////////////
	// range query with $start/$end interpolation
	//////////////////////////////////////////////

	assert ( m_uMinID>0 );
	assert ( m_uMaxID>0 );
	assert ( m_uMinID<=m_uMaxID );
	assert ( sQuery );

	char sValues [ MACRO_COUNT ] [ iBufSize ];
	const char * pValues [ MACRO_COUNT ];
	SphDocID_t uNextID = Min ( m_uCurrentID + (SphDocID_t)m_tParams.m_iRangeStep - 1, m_uMaxID );
	snprintf ( sValues[0], iBufSize, DOCID_FMT, m_uCurrentID );
	snprintf ( sValues[1], iBufSize, DOCID_FMT, uNextID );
	pValues[0] = sValues[0];
	pValues[1] = sValues[1];
	g_iIndexerCurrentRangeMin = m_uCurrentID;
	g_iIndexerCurrentRangeMax = uNextID;
	m_uCurrentID = 1 + uNextID;

	sRes = SubstituteParams ( sQuery, MACRO_VALUES, pValues, MACRO_COUNT );

	// run query
	SqlDismissResult ();
	bool bRes = SqlQuery ( sRes );

	if ( !bRes )
		sError.SetSprintf ( "sql_range_query: %s (DSN=%s)", SqlError(), m_sSqlDSN.cstr() );

	SafeDeleteArray ( sRes );
	return bRes;
}

static bool HookConnect ( const char* szCommand )
{
	FILE * pPipe = popen ( szCommand, "r" );
	if ( !pPipe )
		return false;
	pclose ( pPipe );
	return true;
}

inline static const char* skipspace ( const char* pBuf, const char* pBufEnd )
{
	assert ( pBuf );
	assert ( pBufEnd );

	while ( (pBuf<pBufEnd) && isspace ( *pBuf ) )
		++pBuf;
	return pBuf;
}

inline static const char* scannumber ( const char* pBuf, const char* pBufEnd, SphDocID_t* pRes )
{
	assert ( pBuf );
	assert ( pBufEnd );
	assert ( pRes );

	if ( pBuf<pBufEnd )
	{
		*pRes = 0;
		// FIXME! could check for overflow
		while ( isdigit ( *pBuf ) && pBuf<pBufEnd )
			(*pRes) = 10*(*pRes) + (int)( (*pBuf++)-'0' );
	}
	return pBuf;
}

static bool HookQueryRange ( const char* szCommand, SphDocID_t* pMin, SphDocID_t* pMax )
{
	FILE * pPipe = popen ( szCommand, "r" );
	if ( !pPipe )
		return false;

	const int MAX_BUF_SIZE = 1024;
	char dBuf [MAX_BUF_SIZE];
	int iRead = (int)fread ( dBuf, 1, MAX_BUF_SIZE, pPipe );
	pclose ( pPipe );
	const char* pStart = dBuf;
	const char* pEnd = pStart + iRead;
	// leading whitespace and 1-st number
	pStart = skipspace ( pStart, pEnd );
	pStart = scannumber ( pStart, pEnd, pMin );
	// whitespace and 2-nd number
	pStart = skipspace ( pStart, pEnd );
	scannumber ( pStart, pEnd, pMax );
	return true;
}

static bool HookPostIndex ( const char* szCommand, SphDocID_t uLastIndexed )
{
	const char * sMacro = "$maxid";
	char sValue[32];
	const char* pValue = sValue;
	snprintf ( sValue, sizeof(sValue), DOCID_FMT, uLastIndexed );

	const char * pCmd = SubstituteParams ( szCommand, &sMacro, &pValue, 1 );

	FILE * pPipe = popen ( pCmd, "r" );
	SafeDeleteArray ( pCmd );
	if ( !pPipe )
		return false;
	pclose ( pPipe );
	return true;
}

/// connect to SQL server
bool CSphSource_SQL::Connect ( CSphString & sError )
{
	// do not connect twice
	if ( m_bSqlConnected )
		return true;

	// try to connect
	if ( !SqlConnect() )
	{
		sError.SetSprintf ( "sql_connect: %s (DSN=%s)", SqlError(), m_sSqlDSN.cstr() );
		return false;
	}

	m_tHits.m_dData.Reserve ( m_iMaxHits );

	// all good
	m_bSqlConnected = true;
	if ( !m_tParams.m_sHookConnect.IsEmpty() && !HookConnect ( m_tParams.m_sHookConnect.cstr() ) )
	{
		sError.SetSprintf ( "hook_connect: runtime error %s when running external hook", strerror(errno) );
		return false;
	}
	return true;
}


#define LOC_ERROR(_msg,_arg)			{ sError.SetSprintf ( _msg, _arg ); return false; }
#define LOC_ERROR2(_msg,_arg,_arg2)		{ sError.SetSprintf ( _msg, _arg, _arg2 ); return false; }

/// setup them ranges (called both for document range-queries and MVA range-queries)
bool CSphSource_SQL::SetupRanges ( const char * sRangeQuery, const char * sQuery, const char * sPrefix, CSphString & sError, ERangesReason iReason )
{
	// check step
	if ( m_tParams.m_iRangeStep<=0 )
		LOC_ERROR ( "sql_range_step="INT64_FMT": must be non-zero positive", m_tParams.m_iRangeStep );

	if ( m_tParams.m_iRangeStep<128 )
		sphWarn ( "sql_range_step="INT64_FMT": too small; might hurt indexing performance!", m_tParams.m_iRangeStep );

	// check query for macros
	for ( int i=0; i<MACRO_COUNT; i++ )
		if ( !strstr ( sQuery, MACRO_VALUES[i] ) )
			LOC_ERROR2 ( "%s: macro '%s' not found in match fetch query", sPrefix, MACRO_VALUES[i] );

	// run query
	if ( !SqlQuery ( sRangeQuery ) )
	{
		sError.SetSprintf ( "%s: range-query failed: %s (DSN=%s)", sPrefix, SqlError(), m_sSqlDSN.cstr() );
		return false;
	}

	// fetch min/max
	int iCols = SqlNumFields ();
	if ( iCols!=2 )
		LOC_ERROR2 ( "%s: expected 2 columns (min_id/max_id), got %d", sPrefix, iCols );

	if ( !SqlFetchRow() )
	{
		sError.SetSprintf ( "%s: range-query fetch failed: %s (DSN=%s)", sPrefix, SqlError(), m_sSqlDSN.cstr() );
		return false;
	}

	if ( ( SqlColumn(0)==NULL || !SqlColumn(0)[0] ) && ( SqlColumn(1)==NULL || !SqlColumn(1)[0] ) )
	{
		// the source seems to be empty; workaround
		m_uMinID = 1;
		m_uMaxID = 1;

	} else
	{
		// get and check min/max id
		const char * sCol0 = SqlColumn(0);
		const char * sCol1 = SqlColumn(1);
		m_uMinID = sphToDocid ( sCol0 );
		m_uMaxID = sphToDocid ( sCol1 );
		if ( !sCol0 ) sCol0 = "(null)";
		if ( !sCol1 ) sCol1 = "(null)";

		if ( m_uMinID<=0 )
			LOC_ERROR ( "sql_query_range: min_id='%s': must be positive 32/64-bit unsigned integer", sCol0 );
		if ( m_uMaxID<=0 )
			LOC_ERROR ( "sql_query_range: max_id='%s': must be positive 32/64-bit unsigned integer", sCol1 );
		if ( m_uMinID>m_uMaxID )
			LOC_ERROR2 ( "sql_query_range: min_id='%s', max_id='%s': min_id must be less than max_id", sCol0, sCol1 );
	}

	SqlDismissResult ();

	if ( iReason==SRE_DOCS && ( !m_tParams.m_sHookQueryRange.IsEmpty() ) )
	{
		if ( !HookQueryRange ( m_tParams.m_sHookQueryRange.cstr(), &m_uMinID, &m_uMaxID ) )
			LOC_ERROR ( "hook_query_range: runtime error %s when running external hook", strerror(errno) );
		if ( m_uMinID<=0 )
			LOC_ERROR ( "hook_query_range: min_id="DOCID_FMT": must be positive 32/64-bit unsigned integer", m_uMinID );
		if ( m_uMaxID<=0 )
			LOC_ERROR ( "hook_query_range: max_id="DOCID_FMT": must be positive 32/64-bit unsigned integer", m_uMaxID );
		if ( m_uMinID>m_uMaxID )
			LOC_ERROR2 ( "hook_query_range: min_id="DOCID_FMT", max_id="DOCID_FMT": min_id must be less than max_id", m_uMinID, m_uMaxID );
	}

	return true;
}


/// issue main rows fetch query
bool CSphSource_SQL::IterateStart ( CSphString & sError )
{
	assert ( m_bSqlConnected );

	m_iNullIds = false;
	m_iMaxIds = false;

	// run pre-queries
	ARRAY_FOREACH ( i, m_tParams.m_dQueryPre )
	{
		if ( !SqlQuery ( m_tParams.m_dQueryPre[i].cstr() ) )
		{
			sError.SetSprintf ( "sql_query_pre[%d]: %s (DSN=%s)", i, SqlError(), m_sSqlDSN.cstr() );
			SqlDisconnect ();
			return false;
		}
		SqlDismissResult ();
	}

	for ( ;; )
	{
		m_tParams.m_iRangeStep = 0;

		// issue first fetch query
		if ( !m_tParams.m_sQueryRange.IsEmpty() )
		{
			m_tParams.m_iRangeStep = m_tParams.m_iRefRangeStep;
			// run range-query; setup ranges
			if ( !SetupRanges ( m_tParams.m_sQueryRange.cstr(), m_tParams.m_sQuery.cstr(), "sql_query_range: ", sError, SRE_DOCS ) )
				return false;

			// issue query
			m_uCurrentID = m_uMinID;
			if ( !RunQueryStep ( m_tParams.m_sQuery.cstr(), sError ) )
				return false;
		} else
		{
			// normal query; just issue
			if ( !SqlQuery ( m_tParams.m_sQuery.cstr() ) )
			{
				sError.SetSprintf ( "sql_query: %s (DSN=%s)", SqlError(), m_sSqlDSN.cstr() );
				return false;
			}
		}
		break;
	}

	// some post-query setup
	m_tSchema.Reset();

	for ( int i=0; i<SPH_MAX_FIELDS; i++ )
		m_dUnpack[i] = SPH_UNPACK_NONE;

	m_iSqlFields = SqlNumFields(); // for rowdump
	int iCols = SqlNumFields() - 1; // skip column 0, which must be the id

	CSphVector<bool> dFound;
	dFound.Resize ( m_tParams.m_dAttrs.GetLength() );
	ARRAY_FOREACH ( i, dFound )
		dFound[i] = false;

	const bool bWordDict = m_pDict->GetSettings().m_bWordDict;

	// map plain attrs from SQL
	for ( int i=0; i<iCols; i++ )
	{
		const char * sName = SqlFieldName ( i+1 );
		if ( !sName )
			LOC_ERROR ( "column number %d has no name", i+1 );

		CSphColumnInfo tCol ( sName );
		ARRAY_FOREACH ( j, m_tParams.m_dAttrs )
			if ( !strcasecmp ( tCol.m_sName.cstr(), m_tParams.m_dAttrs[j].m_sName.cstr() ) )
		{
			const CSphColumnInfo & tAttr = m_tParams.m_dAttrs[j];

			tCol.m_eAttrType = tAttr.m_eAttrType;
			assert ( tCol.m_eAttrType!=SPH_ATTR_NONE );

			if ( ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET || tAttr.m_eAttrType==SPH_ATTR_INT64SET ) && tAttr.m_eSrc!=SPH_ATTRSRC_FIELD )
				LOC_ERROR ( "multi-valued attribute '%s' of wrong source-type found in query; must be 'field'", tAttr.m_sName.cstr() );

			tCol = tAttr;
			dFound[j] = true;
			break;
		}

		ARRAY_FOREACH ( j, m_tParams.m_dFileFields )
		{
			if ( !strcasecmp ( tCol.m_sName.cstr(), m_tParams.m_dFileFields[j].cstr() ) )
				tCol.m_bFilename = true;
		}

		tCol.m_iIndex = i+1;
		tCol.m_eWordpart = GetWordpart ( tCol.m_sName.cstr(), bWordDict );

		if ( tCol.m_eAttrType==SPH_ATTR_NONE || tCol.m_bIndexed )
		{
			m_tSchema.m_dFields.Add ( tCol );
			ARRAY_FOREACH ( k, m_tParams.m_dUnpack )
			{
				CSphUnpackInfo & tUnpack = m_tParams.m_dUnpack[k];
				if ( tUnpack.m_sName==tCol.m_sName )
				{
					if ( !m_bCanUnpack )
					{
						sError.SetSprintf ( "this source does not support column unpacking" );
						return false;
					}
					int iIndex = m_tSchema.m_dFields.GetLength() - 1;
					if ( iIndex < SPH_MAX_FIELDS )
					{
						m_dUnpack[iIndex] = tUnpack.m_eFormat;
						m_dUnpackBuffers[iIndex].Resize ( SPH_UNPACK_BUFFER_SIZE );
					}
					break;
				}
			}
		}

		if ( tCol.m_eAttrType!=SPH_ATTR_NONE )
			m_tSchema.AddAttr ( tCol, true ); // all attributes are dynamic at indexing time
	}

	// map multi-valued attrs
	ARRAY_FOREACH ( i, m_tParams.m_dAttrs )
	{
		const CSphColumnInfo & tAttr = m_tParams.m_dAttrs[i];
		if ( ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET || tAttr.m_eAttrType==SPH_ATTR_INT64SET ) && tAttr.m_eSrc!=SPH_ATTRSRC_FIELD )
		{
			CSphColumnInfo tMva;
			tMva = tAttr;
			tMva.m_iIndex = m_tSchema.GetAttrsCount();
			m_tSchema.AddAttr ( tMva, true ); // all attributes are dynamic at indexing time
			dFound[i] = true;
		}
	}

	// warn if some attrs went unmapped
	ARRAY_FOREACH ( i, dFound )
		if ( !dFound[i] )
			sphWarn ( "attribute '%s' not found - IGNORING", m_tParams.m_dAttrs[i].m_sName.cstr() );

	// joined fields
	m_iPlainFieldsLength = m_tSchema.m_dFields.GetLength();

	ARRAY_FOREACH ( i, m_tParams.m_dJoinedFields )
	{
		CSphColumnInfo tCol;
		tCol.m_iIndex = -1;
		tCol.m_sName = m_tParams.m_dJoinedFields[i].m_sName;
		tCol.m_sQuery = m_tParams.m_dJoinedFields[i].m_sQuery;
		tCol.m_bPayload = m_tParams.m_dJoinedFields[i].m_bPayload;
		tCol.m_eSrc = m_tParams.m_dJoinedFields[i].m_sRanged.IsEmpty() ? SPH_ATTRSRC_QUERY : SPH_ATTRSRC_RANGEDQUERY;
		tCol.m_sQueryRange = m_tParams.m_dJoinedFields[i].m_sRanged;
		tCol.m_eWordpart = GetWordpart ( tCol.m_sName.cstr(), bWordDict );
		m_tSchema.m_dFields.Add ( tCol );
	}

	// auto-computed length attributes
	if ( !AddAutoAttrs ( sError ) )
		return false;

	// alloc storage
	AllocDocinfo();

	// check it
	if ( m_tSchema.m_dFields.GetLength()>SPH_MAX_FIELDS )
		LOC_ERROR2 ( "too many fields (fields=%d, max=%d)",
			m_tSchema.m_dFields.GetLength(), SPH_MAX_FIELDS );

	// log it
	if ( m_fpDumpRows )
	{
		const char * sTable = m_tSchema.m_sName.cstr();

		time_t iNow = time ( NULL );
		fprintf ( m_fpDumpRows, "#\n# === source %s ts %d\n# %s#\n", sTable, (int)iNow, ctime ( &iNow ) );
		ARRAY_FOREACH ( i, m_tSchema.m_dFields )
			fprintf ( m_fpDumpRows, "# field %d: %s\n", i, m_tSchema.m_dFields[i].m_sName.cstr() );

		for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
		{
			const CSphColumnInfo & tCol = m_tSchema.GetAttr(i);
			fprintf ( m_fpDumpRows, "# %s = %s # attr %d\n", sphTypeDirective ( tCol.m_eAttrType ), tCol.m_sName.cstr(), i );
		}

		fprintf ( m_fpDumpRows, "#\n\nDROP TABLE IF EXISTS rows_%s;\nCREATE TABLE rows_%s (\n  id VARCHAR(32) NOT NULL,\n",
			sTable, sTable );
		for ( int i=1; i<m_iSqlFields; i++ )
			fprintf ( m_fpDumpRows, "  %s VARCHAR(4096) NOT NULL,\n", SqlFieldName(i) );
		fprintf ( m_fpDumpRows, "  KEY(id) );\n\n" );
	}

	return true;
}

#undef LOC_ERROR
#undef LOC_ERROR2
#undef LOC_SQL_ERROR


void CSphSource_SQL::Disconnect ()
{
	SafeDeleteArray ( m_pReadFileBuffer );
	m_tHits.m_dData.Reset();

	if ( m_iNullIds )
		sphWarn ( "source %s: skipped %d document(s) with zero/NULL ids", m_tSchema.m_sName.cstr(), m_iNullIds );

	if ( m_iMaxIds )
		sphWarn ( "source %s: skipped %d document(s) with DOCID_MAX ids", m_tSchema.m_sName.cstr(), m_iMaxIds );

	m_iNullIds = 0;
	m_iMaxIds = 0;

	if ( m_bSqlConnected )
		SqlDisconnect ();
	m_bSqlConnected = false;
}


BYTE ** CSphSource_SQL::NextDocument ( CSphString & sError )
{
	assert ( m_bSqlConnected );

	// get next non-zero-id row
	do
	{
		// try to get next row
		bool bGotRow = SqlFetchRow ();

		// when the party's over...
		while ( !bGotRow )
		{
			// is that an error?
			if ( SqlIsError() )
			{
				sError.SetSprintf ( "sql_fetch_row: %s", SqlError() );
				m_tDocInfo.m_iDocID = 1; // 0 means legal eof
				return NULL;
			}

			// maybe we can do next step yet?
			if ( !RunQueryStep ( m_tParams.m_sQuery.cstr(), sError ) )
			{
				// if there's a message, there's an error
				// otherwise, we're just over
				if ( !sError.IsEmpty() )
				{
					m_tDocInfo.m_iDocID = 1; // 0 means legal eof
					return NULL;
				}

			} else
			{
				// step went fine; try to fetch
				bGotRow = SqlFetchRow ();
				continue;
			}

			SqlDismissResult ();

			// ok, we're over
			ARRAY_FOREACH ( i, m_tParams.m_dQueryPost )
			{
				if ( !SqlQuery ( m_tParams.m_dQueryPost[i].cstr() ) )
				{
					sphWarn ( "sql_query_post[%d]: error=%s, query=%s",
						i, SqlError(), m_tParams.m_dQueryPost[i].cstr() );
					break;
				}
				SqlDismissResult ();
			}

			m_tDocInfo.m_iDocID = 0; // 0 means legal eof
			return NULL;
		}

		// get him!
		m_tDocInfo.m_iDocID = VerifyID ( sphToDocid ( SqlColumn(0) ) );
		m_uMaxFetchedID = Max ( m_uMaxFetchedID, m_tDocInfo.m_iDocID );
	} while ( !m_tDocInfo.m_iDocID );

	// cleanup attrs
	for ( int i=0; i<m_tSchema.GetRowSize(); i++ )
		m_tDocInfo.m_pDynamic[i] = 0;

	// split columns into fields and attrs
	for ( int i=0; i<m_iPlainFieldsLength; i++ )
	{
		// get that field
		#if USE_ZLIB
		if ( m_dUnpack[i]!=SPH_UNPACK_NONE )
		{
			m_dFields[i] = (BYTE*) SqlUnpackColumn ( i, m_dUnpack[i] );
			continue;
		}
		#endif
		m_dFields[i] = (BYTE*) SqlColumn ( m_tSchema.m_dFields[i].m_iIndex );
	}

	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		const CSphColumnInfo & tAttr = m_tSchema.GetAttr(i); // shortcut

		if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET || tAttr.m_eAttrType==SPH_ATTR_INT64SET )
		{
			int uOff = 0;
			if ( tAttr.m_eSrc==SPH_ATTRSRC_FIELD )
			{
				uOff = ParseFieldMVA ( m_dMva, SqlColumn ( tAttr.m_iIndex ), tAttr.m_eAttrType==SPH_ATTR_INT64SET );
			}
			m_tDocInfo.SetAttr ( tAttr.m_tLocator, uOff );
			continue;
		}

		switch ( tAttr.m_eAttrType )
		{
			case SPH_ATTR_ORDINAL:
			case SPH_ATTR_STRING:
			case SPH_ATTR_JSON:
			case SPH_ATTR_WORDCOUNT:
				// memorize string, fixup NULLs
				m_dStrAttrs[i] = SqlColumn ( tAttr.m_iIndex );
				if ( !m_dStrAttrs[i].cstr() )
					m_dStrAttrs[i] = "";

				m_tDocInfo.SetAttr ( tAttr.m_tLocator, 0 );
				break;

			case SPH_ATTR_FLOAT:
				m_tDocInfo.SetAttrFloat ( tAttr.m_tLocator, sphToFloat ( SqlColumn ( tAttr.m_iIndex ) ) ); // FIXME? report conversion errors maybe?
				break;

			case SPH_ATTR_BIGINT:
				m_tDocInfo.SetAttr ( tAttr.m_tLocator, sphToInt64 ( SqlColumn ( tAttr.m_iIndex ) ) ); // FIXME? report conversion errors maybe?
				break;

			case SPH_ATTR_TOKENCOUNT:
				// reset, and the value will be filled by IterateHits()
				m_tDocInfo.SetAttr ( tAttr.m_tLocator, 0 );
				break;

			default:
				// just store as uint by default
				m_tDocInfo.SetAttr ( tAttr.m_tLocator, sphToDword ( SqlColumn ( tAttr.m_iIndex ) ) ); // FIXME? report conversion errors maybe?
				break;
		}
	}

	// log it
	if ( m_fpDumpRows )
	{
		fprintf ( m_fpDumpRows, "INSERT INTO rows_%s VALUES (", m_tSchema.m_sName.cstr() );
		for ( int i=0; i<m_iSqlFields; i++ )
		{
			if ( i )
				fprintf ( m_fpDumpRows, ", " );
			FormatEscaped ( m_fpDumpRows, SqlColumn(i) );
		}
		fprintf ( m_fpDumpRows, ");\n" );
	}

	return m_dFields;
}


void CSphSource_SQL::PostIndex ()
{
	if ( ( !m_tParams.m_dQueryPostIndex.GetLength() ) && m_tParams.m_sHookPostIndex.IsEmpty() )
		return;

	assert ( !m_bSqlConnected );

	const char * sSqlError = NULL;
	if ( m_tParams.m_dQueryPostIndex.GetLength() )
	{
#define LOC_SQL_ERROR(_msg) { sSqlError = _msg; break; }

		for ( ;; )
		{
			if ( !SqlConnect () )
				LOC_SQL_ERROR ( "mysql_real_connect" );

			ARRAY_FOREACH ( i, m_tParams.m_dQueryPostIndex )
			{
				char * sQuery = sphStrMacro ( m_tParams.m_dQueryPostIndex[i].cstr(), "$maxid", m_uMaxFetchedID );
				bool bRes = SqlQuery ( sQuery );
				delete [] sQuery;

				if ( !bRes )
					LOC_SQL_ERROR ( "sql_query_post_index" );

				SqlDismissResult ();
			}

			break;
		}

		if ( sSqlError )
			sphWarn ( "%s: %s (DSN=%s)", sSqlError, SqlError(), m_sSqlDSN.cstr() );

#undef LOC_SQL_ERROR

		SqlDisconnect ();
	}
	if ( !m_tParams.m_sHookPostIndex.IsEmpty() && !HookPostIndex ( m_tParams.m_sHookPostIndex.cstr(), m_uMaxFetchedID ) )
	{
		sphWarn ( "hook_post_index: runtime error %s when running external hook", strerror(errno) );
	}
}


bool CSphSource_SQL::IterateMultivaluedStart ( int iAttr, CSphString & sError )
{
	if ( iAttr<0 || iAttr>=m_tSchema.GetAttrsCount() )
		return false;

	m_iMultiAttr = iAttr;
	const CSphColumnInfo & tAttr = m_tSchema.GetAttr(iAttr);

	if ( !(tAttr.m_eAttrType==SPH_ATTR_UINT32SET || tAttr.m_eAttrType==SPH_ATTR_INT64SET ) )
		return false;

	CSphString sPrefix;
	switch ( tAttr.m_eSrc )
	{
	case SPH_ATTRSRC_FIELD:
		return false;

	case SPH_ATTRSRC_QUERY:
		// run simple query
		if ( !SqlQuery ( tAttr.m_sQuery.cstr() ) )
		{
			sError.SetSprintf ( "multi-valued attr '%s' query failed: %s", tAttr.m_sName.cstr(), SqlError() );
			return false;
		}
		break;

	case SPH_ATTRSRC_RANGEDQUERY:
			m_tParams.m_iRangeStep = m_tParams.m_iRefRangeStep;

			// setup ranges
			sPrefix.SetSprintf ( "multi-valued attr '%s' ranged query: ", tAttr.m_sName.cstr() );
			if ( !SetupRanges ( tAttr.m_sQueryRange.cstr(), tAttr.m_sQuery.cstr(), sPrefix.cstr(), sError, SRE_MVA ) )
				return false;

			// run first step (in order to report errors)
			m_uCurrentID = m_uMinID;
			if ( !RunQueryStep ( tAttr.m_sQuery.cstr(), sError ) )
				return false;

			break;

	default:
		sError.SetSprintf ( "INTERNAL ERROR: unknown multi-valued attr source type %d", tAttr.m_eSrc );
		return false;
	}

	// check fields count
	if ( SqlNumFields()!=2 )
	{
		sError.SetSprintf ( "multi-valued attr '%s' query returned %d fields (expected 2)", tAttr.m_sName.cstr(), SqlNumFields() );
		SqlDismissResult ();
		return false;
	}
	return true;
}


bool CSphSource_SQL::IterateMultivaluedNext ()
{
	const CSphColumnInfo & tAttr = m_tSchema.GetAttr ( m_iMultiAttr );

	assert ( m_bSqlConnected );
	assert ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET || tAttr.m_eAttrType==SPH_ATTR_INT64SET );

	// fetch next row
	bool bGotRow = SqlFetchRow ();
	while ( !bGotRow )
	{
		if ( SqlIsError() )
			sphDie ( "sql_fetch_row: %s", SqlError() ); // FIXME! this should be reported

		if ( tAttr.m_eSrc!=SPH_ATTRSRC_RANGEDQUERY )
		{
			SqlDismissResult();
			return false;
		}

		CSphString sTmp;
		if ( !RunQueryStep ( tAttr.m_sQuery.cstr(), sTmp ) ) // FIXME! this should be reported
			return false;

		bGotRow = SqlFetchRow ();
		continue;
	}

	// return that tuple or offset to storage for MVA64 value
	m_tDocInfo.m_iDocID = sphToDocid ( SqlColumn(0) );
	m_dMva.Resize ( 0 );
	if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET )
		m_dMva.Add ( sphToDword ( SqlColumn(1) ) );
	else
		sphAddMva64 ( m_dMva, sphToInt64 ( SqlColumn(1) ) );

	return true;
}


bool CSphSource_SQL::IterateKillListStart ( CSphString & sError )
{
	if ( m_tParams.m_sQueryKilllist.IsEmpty () )
		return false;

	if ( !SqlQuery ( m_tParams.m_sQueryKilllist.cstr () ) )
	{
		sError.SetSprintf ( "killlist query failed: %s", SqlError() );
		return false;
	}

	return true;
}


bool CSphSource_SQL::IterateKillListNext ( SphDocID_t & tDocId )
{
	if ( SqlFetchRow () )
		tDocId = sphToDocid ( SqlColumn(0) );
	else
	{
		if ( SqlIsError() )
			sphDie ( "sql_query_killlist: %s", SqlError() ); // FIXME! this should be reported
		else
		{
			SqlDismissResult ();
			return false;
		}
	}

	return true;
}


void CSphSource_SQL::ReportUnpackError ( int iIndex, int iError )
{
	if ( !m_bUnpackFailed )
	{
		m_bUnpackFailed = true;
		sphWarn ( "failed to unpack column '%s', error=%d, docid=" DOCID_FMT, SqlFieldName(iIndex), iError, m_tDocInfo.m_iDocID );
	}
}


#if !USE_ZLIB

const char * CSphSource_SQL::SqlUnpackColumn ( int iFieldIndex, ESphUnpackFormat )
{
	return SqlColumn ( m_tSchema.m_dFields[iFieldIndex].m_iIndex );
}

#else

const char * CSphSource_SQL::SqlUnpackColumn ( int iFieldIndex, ESphUnpackFormat eFormat )
{
	int iIndex = m_tSchema.m_dFields[iFieldIndex].m_iIndex;
	const char * pData = SqlColumn(iIndex);

	if ( pData==NULL )
		return NULL;

	int iPackedLen = SqlColumnLength(iIndex);
	if ( iPackedLen<=0 )
		return NULL;


	CSphVector<char> & tBuffer = m_dUnpackBuffers[iFieldIndex];
	switch ( eFormat )
	{
		case SPH_UNPACK_MYSQL_COMPRESS:
		{
			if ( iPackedLen<=4 )
			{
				if ( !m_bUnpackFailed )
				{
					m_bUnpackFailed = true;
					sphWarn ( "failed to unpack '%s', invalid column size (size=%d), "
						"docid="DOCID_FMT, SqlFieldName(iIndex), iPackedLen, m_tDocInfo.m_iDocID );
				}
				return NULL;
			}

			unsigned long uSize = 0;
			for ( int i=0; i<4; i++ )
				uSize += ((unsigned long)((BYTE)pData[i])) << ( 8*i );
			uSize &= 0x3FFFFFFF;

			if ( uSize > m_tParams.m_uUnpackMemoryLimit )
			{
				if ( !m_bUnpackOverflow )
				{
					m_bUnpackOverflow = true;
					sphWarn ( "failed to unpack '%s', column size limit exceeded (size=%d),"
						" docid="DOCID_FMT, SqlFieldName(iIndex), (int)uSize, m_tDocInfo.m_iDocID );
				}
				return NULL;
			}

			int iResult;
			tBuffer.Resize ( uSize + 1 );
			unsigned long uLen = iPackedLen-4;
			iResult = uncompress ( (Bytef *)tBuffer.Begin(), &uSize, (Bytef *)pData + 4, uLen );
			if ( iResult==Z_OK )
			{
				tBuffer[uSize] = 0;
				return &tBuffer[0];
			} else
				ReportUnpackError ( iIndex, iResult );
			return NULL;
		}

		case SPH_UNPACK_ZLIB:
		{
			char * sResult = 0;
			int iBufferOffset = 0;
			int iResult;

			z_stream tStream;
			tStream.zalloc = Z_NULL;
			tStream.zfree = Z_NULL;
			tStream.opaque = Z_NULL;
			tStream.avail_in = iPackedLen;
			tStream.next_in = (Bytef *)SqlColumn(iIndex);

			iResult = inflateInit ( &tStream );
			if ( iResult!=Z_OK )
				return NULL;

			for ( ;; )
			{
				tStream.next_out = (Bytef *)&tBuffer[iBufferOffset];
				tStream.avail_out = tBuffer.GetLength() - iBufferOffset - 1;

				iResult = inflate ( &tStream, Z_NO_FLUSH );
				if ( iResult==Z_STREAM_END )
				{
					tBuffer [ tStream.total_out ] = 0;
					sResult = &tBuffer[0];
					break;
				} else if ( iResult==Z_OK )
				{
					assert ( tStream.avail_out==0 );

					tBuffer.Resize ( tBuffer.GetLength()*2 );
					iBufferOffset = tStream.total_out;
				} else
				{
					ReportUnpackError ( iIndex, iResult );
					break;
				}
			}

			inflateEnd ( &tStream );
			return sResult;
		}

		case SPH_UNPACK_NONE:
			return pData;
	}
	return NULL;
}
#endif // USE_ZLIB


ISphHits * CSphSource_SQL::IterateJoinedHits ( CSphString & sError )
{
	// iterating of joined hits happens after iterating hits from main query
	// so we may be sure at this moment no new IDs will be put in m_dAllIds
	if ( !m_bIdsSorted )
	{
		m_dAllIds.Uniq();
		m_bIdsSorted = true;
	}
	m_tHits.m_dData.Resize ( 0 );

	// eof check
	if ( m_iJoinedHitField>=m_tSchema.m_dFields.GetLength() )
	{
		m_tDocInfo.m_iDocID = 0;
		return &m_tHits;
	}

	bool bProcessingRanged = true;

	// my fetch loop
	while ( m_iJoinedHitField<m_tSchema.m_dFields.GetLength() )
	{
		if ( m_tState.m_bProcessingHits || SqlFetchRow() )
		{
			// next row
			m_tDocInfo.m_iDocID = sphToDocid ( SqlColumn(0) ); // FIXME! handle conversion errors and zero/max values?

			// lets skip joined document totally if there was no such document ID returned by main query
			if ( !m_dAllIds.BinarySearch ( m_tDocInfo.m_iDocID ) )
				continue;

			// field start? restart ids
			if ( !m_iJoinedHitID )
				m_iJoinedHitID = m_tDocInfo.m_iDocID;

			// docid asc requirement violated? report an error
			if ( m_iJoinedHitID>m_tDocInfo.m_iDocID )
			{
				sError.SetSprintf ( "joined field '%s': query MUST return document IDs in ASC order",
					m_tSchema.m_dFields[m_iJoinedHitField].m_sName.cstr() );
				return NULL;
			}

			// next document? update tracker, reset position
			if ( m_iJoinedHitID<m_tDocInfo.m_iDocID )
			{
				m_iJoinedHitID = m_tDocInfo.m_iDocID;
				m_iJoinedHitPos = 0;
			}

			if ( !m_tState.m_bProcessingHits )
			{
				m_tState = CSphBuildHitsState_t();
				m_tState.m_iField = m_iJoinedHitField;
				m_tState.m_iStartField = m_iJoinedHitField;
				m_tState.m_iEndField = m_iJoinedHitField+1;

				if ( m_tSchema.m_dFields[m_iJoinedHitField].m_bPayload )
					m_tState.m_iStartPos = sphToDword ( SqlColumn(2) );
				else
					m_tState.m_iStartPos = m_iJoinedHitPos;
			}

			// build those hits
			BYTE * dText[] = { (BYTE *)SqlColumn(1) };
			m_tState.m_dFields = dText;

			BuildHits ( sError, true );

			// update current position
			if ( !m_tSchema.m_dFields[m_iJoinedHitField].m_bPayload && !m_tState.m_bProcessingHits && m_tHits.Length() )
				m_iJoinedHitPos = HITMAN::GetPos ( m_tHits.Last()->m_iWordPos );

			if ( m_tState.m_bProcessingHits )
				break;
		} else if ( SqlIsError() )
		{
			// error while fetching row
			sError = SqlError();
			return NULL;

		} else
		{
			int iLastField = m_iJoinedHitField;
			bool bRanged = ( m_iJoinedHitField>=m_iPlainFieldsLength && m_iJoinedHitField<m_tSchema.m_dFields.GetLength()
				&& m_tSchema.m_dFields[m_iJoinedHitField].m_eSrc==SPH_ATTRSRC_RANGEDQUERY );

			// current field is over, continue to next field
			if ( m_iJoinedHitField<0 )
				m_iJoinedHitField = m_iPlainFieldsLength;
			else if ( !bRanged || !bProcessingRanged )
				m_iJoinedHitField++;

			// eof check
			if ( m_iJoinedHitField>=m_tSchema.m_dFields.GetLength() )
			{
				m_tDocInfo.m_iDocID = ( m_tHits.Length() ? 1 : 0 ); // to eof or not to eof
				return &m_tHits;
			}

			SqlDismissResult ();

			bProcessingRanged = false;
			bool bCheckNumFields = true;
			CSphColumnInfo & tJoined = m_tSchema.m_dFields[m_iJoinedHitField];

			// start fetching next field
			if ( tJoined.m_eSrc!=SPH_ATTRSRC_RANGEDQUERY )
			{
				if ( !SqlQuery ( tJoined.m_sQuery.cstr() ) )
				{
					sError = SqlError();
					return NULL;
				}
			} else
			{
				m_tParams.m_iRangeStep = m_tParams.m_iRefRangeStep;

				// setup ranges for next field
				if ( iLastField!=m_iJoinedHitField )
				{
					CSphString sPrefix;
					sPrefix.SetSprintf ( "joined field '%s' ranged query: ", tJoined.m_sName.cstr() );
					if ( !SetupRanges ( tJoined.m_sQueryRange.cstr(), tJoined.m_sQuery.cstr(), sPrefix.cstr(), sError, SRE_JOINEDHITS ) )
						return NULL;

					m_uCurrentID = m_uMinID;
				}

				// run first step (in order to report errors)
				bool bRes = RunQueryStep ( tJoined.m_sQuery.cstr(), sError );
				bProcessingRanged = bRes; // select next documents in range or loop once to process next field
				bCheckNumFields = bRes;

				if ( !sError.IsEmpty() )
					return NULL;
			}

			const int iExpected = m_tSchema.m_dFields[m_iJoinedHitField].m_bPayload ? 3 : 2;
			if ( bCheckNumFields && SqlNumFields()!=iExpected )
			{
				const char * sName = m_tSchema.m_dFields[m_iJoinedHitField].m_sName.cstr();
				sError.SetSprintf ( "joined field '%s': query MUST return exactly %d columns, got %d", sName, iExpected, SqlNumFields() );
				return NULL;
			}

			m_iJoinedHitID = 0;
			m_iJoinedHitPos = 0;
		}
	}

	return &m_tHits;
}

/////////////////////////////////////////////////////////////////////////////
// MYSQL SOURCE
/////////////////////////////////////////////////////////////////////////////

#if USE_MYSQL

CSphSourceParams_MySQL::CSphSourceParams_MySQL ()
	: m_iFlags ( 0 )
{
	m_iPort = 3306;
}


CSphSource_MySQL::CSphSource_MySQL ( const char * sName )
	: CSphSource_SQL	( sName )
	, m_pMysqlResult	( NULL )
	, m_pMysqlFields	( NULL )
	, m_tMysqlRow		( NULL )
	, m_pMysqlLengths	( NULL )
{
	m_bCanUnpack = true;
}


void CSphSource_MySQL::SqlDismissResult ()
{
	if ( !m_pMysqlResult )
		return;

	while ( m_pMysqlResult )
	{
		mysql_free_result ( m_pMysqlResult );
		m_pMysqlResult = NULL;

		// stored procedures might return multiple result sets
		// FIXME? we might want to index all of them
		// but for now, let's simply dismiss additional result sets
		if ( mysql_next_result ( &m_tMysqlDriver )==0 )
		{
			m_pMysqlResult = mysql_use_result ( &m_tMysqlDriver );

			static bool bOnce = false;
			if ( !bOnce && m_pMysqlResult && mysql_num_rows ( m_pMysqlResult ) )
			{
				sphWarn ( "indexing of multiple result sets is not supported yet; some results sets were dismissed!" );
				bOnce = true;
			}
		}
	}

	m_pMysqlFields = NULL;
	m_pMysqlLengths = NULL;
}


bool CSphSource_MySQL::SqlQuery ( const char * sQuery )
{
	if ( mysql_query ( &m_tMysqlDriver, sQuery ) )
	{
		if ( m_tParams.m_bPrintQueries )
			fprintf ( stdout, "SQL-QUERY: %s: FAIL\n", sQuery );
		return false;
	}
	if ( m_tParams.m_bPrintQueries )
		fprintf ( stdout, "SQL-QUERY: %s: ok\n", sQuery );

	m_pMysqlResult = mysql_use_result ( &m_tMysqlDriver );
	m_pMysqlFields = NULL;
	return true;
}


bool CSphSource_MySQL::SqlIsError ()
{
	return mysql_errno ( &m_tMysqlDriver )!=0;
}


const char * CSphSource_MySQL::SqlError ()
{
	return mysql_error ( &m_tMysqlDriver );
}


bool CSphSource_MySQL::SqlConnect ()
{
	mysql_init ( &m_tMysqlDriver );

	if ( !m_sSslKey.IsEmpty() || !m_sSslCert.IsEmpty() || !m_sSslCA.IsEmpty() )
		mysql_ssl_set ( &m_tMysqlDriver, m_sSslKey.cstr(), m_sSslCert.cstr(), m_sSslCA.cstr(), NULL, NULL );

	m_iMysqlConnectFlags |= CLIENT_MULTI_RESULTS; // we now know how to handle this
	bool bRes = ( NULL!=mysql_real_connect ( &m_tMysqlDriver,
		m_tParams.m_sHost.cstr(), m_tParams.m_sUser.cstr(), m_tParams.m_sPass.cstr(),
		m_tParams.m_sDB.cstr(), m_tParams.m_iPort, m_sMysqlUsock.cstr(), m_iMysqlConnectFlags ) );
	if ( m_tParams.m_bPrintQueries )
		fprintf ( stdout, bRes ? "SQL-CONNECT: ok\n" : "SQL-CONNECT: FAIL\n" );
	return bRes;
}


void CSphSource_MySQL::SqlDisconnect ()
{
	if ( m_tParams.m_bPrintQueries )
		fprintf ( stdout, "SQL-DISCONNECT\n" );

	mysql_close ( &m_tMysqlDriver );
}


int CSphSource_MySQL::SqlNumFields ()
{
	if ( !m_pMysqlResult )
		return -1;

	return mysql_num_fields ( m_pMysqlResult );
}


bool CSphSource_MySQL::SqlFetchRow ()
{
	if ( !m_pMysqlResult )
		return false;

	m_tMysqlRow = mysql_fetch_row ( m_pMysqlResult );
	return m_tMysqlRow!=NULL;
}


const char * CSphSource_MySQL::SqlColumn ( int iIndex )
{
	if ( !m_pMysqlResult )
		return NULL;

	return m_tMysqlRow[iIndex];
}


const char * CSphSource_MySQL::SqlFieldName ( int iIndex )
{
	if ( !m_pMysqlResult )
		return NULL;

	if ( !m_pMysqlFields )
		m_pMysqlFields = mysql_fetch_fields ( m_pMysqlResult );

	return m_pMysqlFields[iIndex].name;
}


DWORD CSphSource_MySQL::SqlColumnLength ( int iIndex )
{
	if ( !m_pMysqlResult )
		return 0;

	if ( !m_pMysqlLengths )
		m_pMysqlLengths = mysql_fetch_lengths ( m_pMysqlResult );

	return m_pMysqlLengths[iIndex];
}


bool CSphSource_MySQL::Setup ( const CSphSourceParams_MySQL & tParams )
{
	if ( !CSphSource_SQL::Setup ( tParams ) )
		return false;

	m_sMysqlUsock = tParams.m_sUsock;
	m_iMysqlConnectFlags = tParams.m_iFlags;
	m_sSslKey = tParams.m_sSslKey;
	m_sSslCert = tParams.m_sSslCert;
	m_sSslCA = tParams.m_sSslCA;

	// build and store DSN for error reporting
	char sBuf [ 1024 ];
	snprintf ( sBuf, sizeof(sBuf), "mysql%s", m_sSqlDSN.cstr()+3 );
	m_sSqlDSN = sBuf;

	return true;
}

#endif // USE_MYSQL

/////////////////////////////////////////////////////////////////////////////
// PGSQL SOURCE
/////////////////////////////////////////////////////////////////////////////

#if USE_PGSQL

CSphSourceParams_PgSQL::CSphSourceParams_PgSQL ()
{
	m_iRangeStep = 1024;
	m_iPort = 5432;
}


CSphSource_PgSQL::CSphSource_PgSQL ( const char * sName )
	: CSphSource_SQL	( sName )
	, m_pPgResult		( NULL )
	, m_iPgRows			( 0 )
	, m_iPgRow			( 0 )
{
}


bool CSphSource_PgSQL::SqlIsError ()
{
	return ( m_iPgRow<m_iPgRows ); // if we're over, it's just last row
}


const char * CSphSource_PgSQL::SqlError ()
{
	return PQerrorMessage ( m_tPgDriver );
}


bool CSphSource_PgSQL::Setup ( const CSphSourceParams_PgSQL & tParams )
{
	// checks
	CSphSource_SQL::Setup ( tParams );

	m_sPgClientEncoding = tParams.m_sClientEncoding;
	if ( !m_sPgClientEncoding.cstr() )
		m_sPgClientEncoding = "";

	// build and store DSN for error reporting
	char sBuf [ 1024 ];
	snprintf ( sBuf, sizeof(sBuf), "pgsql%s", m_sSqlDSN.cstr()+3 );
	m_sSqlDSN = sBuf;

	return true;
}


bool CSphSource_PgSQL::IterateStart ( CSphString & sError )
{
	bool bResult = CSphSource_SQL::IterateStart ( sError );
	if ( !bResult )
		return false;

	int iMaxIndex = 0;
	for ( int i = 0; i < m_tSchema.GetAttrsCount(); i++ )
		iMaxIndex = Max ( iMaxIndex, m_tSchema.GetAttr(i).m_iIndex );

	ARRAY_FOREACH ( i, m_tSchema.m_dFields )
		iMaxIndex = Max ( iMaxIndex, m_tSchema.m_dFields[i].m_iIndex );

	m_dIsColumnBool.Resize ( iMaxIndex + 1 );
	ARRAY_FOREACH ( i, m_dIsColumnBool )
		m_dIsColumnBool[i] = false;

	for ( int i = 0; i < m_tSchema.GetAttrsCount(); i++ )
		m_dIsColumnBool [ m_tSchema.GetAttr(i).m_iIndex ] = ( m_tSchema.GetAttr(i).m_eAttrType==SPH_ATTR_BOOL );

	return true;
}


bool CSphSource_PgSQL::SqlConnect ()
{
	char sPort[64];
	snprintf ( sPort, sizeof(sPort), "%d", m_tParams.m_iPort );
	m_tPgDriver = PQsetdbLogin ( m_tParams.m_sHost.cstr(), sPort, NULL, NULL,
		m_tParams.m_sDB.cstr(), m_tParams.m_sUser.cstr(), m_tParams.m_sPass.cstr() );

	if ( PQstatus ( m_tPgDriver )==CONNECTION_BAD )
	{
		if ( m_tParams.m_bPrintQueries )
			fprintf ( stdout, "SQL-CONNECT: FAIL\n" );
		return false;
	}

	// set client encoding
	if ( !m_sPgClientEncoding.IsEmpty() )
		if ( -1==PQsetClientEncoding ( m_tPgDriver, m_sPgClientEncoding.cstr() ) )
	{
		SqlDisconnect ();
		if ( m_tParams.m_bPrintQueries )
			fprintf ( stdout, "SQL-CONNECT: FAIL\n" );
		return false;
	}

	if ( m_tParams.m_bPrintQueries )
		fprintf ( stdout, "SQL-CONNECT: ok\n" );
	return true;
}


void CSphSource_PgSQL::SqlDisconnect ()
{
	if ( m_tParams.m_bPrintQueries )
		fprintf ( stdout, "SQL-DISCONNECT\n" );

	PQfinish ( m_tPgDriver );
}


bool CSphSource_PgSQL::SqlQuery ( const char * sQuery )
{
	m_iPgRow = -1;
	m_iPgRows = 0;

	m_pPgResult = PQexec ( m_tPgDriver, sQuery );

	ExecStatusType eRes = PQresultStatus ( m_pPgResult );
	if ( ( eRes!=PGRES_COMMAND_OK ) && ( eRes!=PGRES_TUPLES_OK ) )
	{
		if ( m_tParams.m_bPrintQueries )
			fprintf ( stdout, "SQL-QUERY: %s: FAIL\n", sQuery );
		return false;
	}
	if ( m_tParams.m_bPrintQueries )
		fprintf ( stdout, "SQL-QUERY: %s: ok\n", sQuery );

	m_iPgRows = PQntuples ( m_pPgResult );
	return true;
}


void CSphSource_PgSQL::SqlDismissResult ()
{
	if ( !m_pPgResult )
		return;

	PQclear ( m_pPgResult );
	m_pPgResult = NULL;
}


int CSphSource_PgSQL::SqlNumFields ()
{
	if ( !m_pPgResult )
		return -1;

	return PQnfields ( m_pPgResult );
}


const char * CSphSource_PgSQL::SqlColumn ( int iIndex )
{
	if ( !m_pPgResult )
		return NULL;

	const char * szValue = PQgetvalue ( m_pPgResult, m_iPgRow, iIndex );
	if ( m_dIsColumnBool.GetLength() && m_dIsColumnBool[iIndex] && szValue[0]=='t' && !szValue[1] )
		return "1";

	return szValue;
}


const char * CSphSource_PgSQL::SqlFieldName ( int iIndex )
{
	if ( !m_pPgResult )
		return NULL;

	return PQfname ( m_pPgResult, iIndex );
}


bool CSphSource_PgSQL::SqlFetchRow ()
{
	if ( !m_pPgResult )
		return false;
	return ( ++m_iPgRow<m_iPgRows );
}


DWORD CSphSource_PgSQL::SqlColumnLength ( int )
{
	return 0;
}

#endif // USE_PGSQL

/////////////////////////////////////////////////////////////////////////////
// XMLPIPE (v2)
/////////////////////////////////////////////////////////////////////////////

template < typename T >
struct CSphSchemaConfigurator
{
	void ConfigureAttrs ( const CSphVariant * pHead, ESphAttr eAttrType, CSphSchema & tSchema ) const
	{
		for ( const CSphVariant * pCur = pHead; pCur; pCur= pCur->m_pNext )
		{
			CSphColumnInfo tCol ( pCur->cstr(), eAttrType );
			char * pColon = strchr ( const_cast<char*> ( tCol.m_sName.cstr() ), ':' );
			if ( pColon )
			{
				*pColon = '\0';

				if ( eAttrType==SPH_ATTR_INTEGER )
				{
					int iBits = strtol ( pColon+1, NULL, 10 );
					if ( iBits<=0 || iBits>ROWITEM_BITS )
					{
						sphWarn ( "%s", ((T*)this)->DecorateMessage ( "attribute '%s': invalid bitcount=%d (bitcount ignored)", tCol.m_sName.cstr(), iBits ) );
						iBits = -1;
					}

					tCol.m_tLocator.m_iBitCount = iBits;
				} else
				{
					sphWarn ( "%s", ((T*)this)->DecorateMessage ( "attribute '%s': bitcount is only supported for integer types", tCol.m_sName.cstr() ) );
				}
			}

			tCol.m_iIndex = tSchema.GetAttrsCount ();

			if ( eAttrType==SPH_ATTR_UINT32SET || eAttrType==SPH_ATTR_INT64SET )
			{
				tCol.m_eAttrType = eAttrType;
				tCol.m_eSrc = SPH_ATTRSRC_FIELD;
			}

			tSchema.AddAttr ( tCol, true ); // all attributes are dynamic at indexing time
		}
	}

	void ConfigureFields ( const CSphVariant * pHead, bool bWordDict, CSphSchema & tSchema ) const
	{
		for ( const CSphVariant * pCur = pHead; pCur; pCur= pCur->m_pNext )
		{
			const char * sFieldName = pCur->cstr();

			bool bFound = false;
			for ( int i = 0; i < tSchema.m_dFields.GetLength () && !bFound; i++ )
				bFound = ( tSchema.m_dFields[i].m_sName==sFieldName );

			if ( bFound )
				sphWarn ( "%s", ((T*)this)->DecorateMessage ( "duplicate field '%s'", sFieldName ) );
			else
				AddFieldToSchema ( sFieldName, bWordDict, tSchema );
		}
	}

	void AddFieldToSchema ( const char * sFieldName, bool bWordDict, CSphSchema & tSchema ) const
	{
		CSphColumnInfo tCol ( sFieldName );
		tCol.m_eWordpart = ((T*)this)->GetWordpart ( tCol.m_sName.cstr(), bWordDict );
		tSchema.m_dFields.Add ( tCol );
	}
};


#if USE_LIBEXPAT

/// XML pipe source implementation (v2)
class CSphSource_XMLPipe2 : public CSphSource_Document, public CSphSchemaConfigurator<CSphSource_XMLPipe2>
{
public:
	explicit			CSphSource_XMLPipe2 ( const char * sName );
					~CSphSource_XMLPipe2 ();

	bool			Setup ( int iFieldBufferMax, bool bFixupUTF8, FILE * pPipe, const CSphConfigSection & hSource );			///< memorize the command
	virtual bool	Connect ( CSphString & sError );			///< run the command and open the pipe
	virtual void	Disconnect ();								///< close the pipe

	virtual bool	IterateStart ( CSphString & ) { m_iPlainFieldsLength = m_tSchema.m_dFields.GetLength(); return true; }	///< Connect() starts getting documents automatically, so this one is empty
	virtual BYTE **	NextDocument ( CSphString & sError );			///< parse incoming chunk and emit some hits

	virtual bool	HasAttrsConfigured ()							{ return true; }	///< xmlpipe always has some attrs for now
	virtual bool	IterateMultivaluedStart ( int, CSphString & )	{ return false; }
	virtual bool	IterateMultivaluedNext ()						{ return false; }
	virtual bool	IterateKillListStart ( CSphString & );
	virtual bool	IterateKillListNext ( SphDocID_t & tDocId );

	void			StartElement ( const char * szName, const char ** pAttrs );
	void			EndElement ( const char * pName );
	void			Characters ( const char * pCharacters, int iLen );

	void			Error ( const char * sTemplate, ... ) __attribute__ ( ( format ( printf, 2, 3 ) ) );
	const char *	DecorateMessage ( const char * sTemplate, ... ) const __attribute__ ( ( format ( printf, 2, 3 ) ) );
	const char *	DecorateMessageVA ( const char * sTemplate, va_list ap ) const;

private:
	struct Document_t
	{
		SphDocID_t					m_iDocID;
		CSphVector < CSphVector<BYTE> >	m_dFields;
		CSphVector<CSphString>		m_dAttrs;
	};

	Document_t *				m_pCurDocument;
	CSphVector<Document_t *>	m_dParsedDocuments;

	FILE *			m_pPipe;			///< incoming stream
	CSphString		m_sError;
	CSphVector<CSphString> m_dDefaultAttrs;
	CSphVector<CSphString> m_dInvalid;
	CSphVector<CSphString> m_dWarned;
	int				m_iElementDepth;

	BYTE *			m_pBuffer;
	int				m_iBufferSize;

	CSphVector<BYTE*>m_dFieldPtrs;
	bool			m_bRemoveParsed;

	bool			m_bInDocset;
	bool			m_bInSchema;
	bool			m_bInDocument;
	bool			m_bInKillList;
	bool			m_bInId;
	bool			m_bInIgnoredTag;
	bool			m_bFirstTagAfterDocset;

	int				m_iKillListIterator;
	CSphVector < SphDocID_t > m_dKillList;

	int				m_iMVA;
	int				m_iMVAIterator;
	CSphVector < CSphVector <DWORD> > m_dFieldMVAs;
	CSphVector < int > m_dAttrToMVA;

	int				m_iCurField;
	int				m_iCurAttr;

	XML_Parser		m_pParser;

	int				m_iFieldBufferMax;
	BYTE * 			m_pFieldBuffer;
	int				m_iFieldBufferLen;

	bool			m_bFixupUTF8;		///< whether to replace invalid utf-8 codepoints with spaces
	int				m_iReparseStart;	///< utf-8 fixerupper might need to postpone a few bytes, starting at this offset
	int				m_iReparseLen;		///< and this much bytes (under 4)

	void			UnexpectedCharaters ( const char * pCharacters, int iLen, const char * szComment );

	bool			ParseNextChunk ( int iBufferLen, CSphString & sError );

	void DocumentError ( const char * sWhere )
	{
		Error ( "malformed source, <sphinx:document> found inside %s", sWhere );

		// Ideally I'd like to display a notice on the next line that
		// would say where exactly it's allowed. E.g.:
		//
		// <sphinx:document> must be contained in <sphinx:docset>
	}
};


// callbacks
static void XMLCALL xmlStartElement ( void * user_data, const XML_Char * name, const XML_Char ** attrs )
{
	CSphSource_XMLPipe2 * pSource = (CSphSource_XMLPipe2 *) user_data;
	pSource->StartElement ( name, attrs );
}


static void XMLCALL xmlEndElement ( void * user_data, const XML_Char * name )
{
	CSphSource_XMLPipe2 * pSource = (CSphSource_XMLPipe2 *) user_data;
	pSource->EndElement ( name );
}


static void XMLCALL xmlCharacters ( void * user_data, const XML_Char * ch, int len )
{
	CSphSource_XMLPipe2 * pSource = (CSphSource_XMLPipe2 *) user_data;
	pSource->Characters ( ch, len );
}

#if USE_LIBICONV
static int XMLCALL xmlUnknownEncoding ( void *, const XML_Char * name, XML_Encoding * info )
{
	iconv_t pDesc = iconv_open ( "UTF-16", name );
	if ( !pDesc )
		return XML_STATUS_ERROR;

	for ( size_t i = 0; i < 256; i++ )
	{
		char cIn = (char) i;
		char dOut[4];
		memset ( dOut, 0, sizeof ( dOut ) );
#if ICONV_INBUF_CONST
		const char * pInbuf = &cIn;
#else
		char * pInbuf = &cIn;
#endif
		char * pOutbuf = dOut;
		size_t iInBytesLeft = 1;
		size_t iOutBytesLeft = 4;

		if ( iconv ( pDesc, &pInbuf, &iInBytesLeft, &pOutbuf, &iOutBytesLeft )!=size_t(-1) )
			info->map[i] = int ( BYTE ( dOut[0] ) ) << 8 | int ( BYTE ( dOut[1] ) );
		else
			info->map[i] = 0;
	}

	iconv_close ( pDesc );

	return XML_STATUS_OK;
}
#endif

CSphSource_XMLPipe2::CSphSource_XMLPipe2 ( const char * sName )
	: CSphSource_Document ( sName )
	, m_pCurDocument	( NULL )
	, m_pPipe			( NULL )
	, m_iElementDepth	( 0 )
	, m_pBuffer			( NULL )
	, m_iBufferSize		( 1048576 )
	, m_bRemoveParsed	( false )
	, m_bInDocset		( false )
	, m_bInSchema		( false )
	, m_bInDocument		( false )
	, m_bInKillList		( false )
	, m_bInId			( false )
	, m_bInIgnoredTag	( false )
	, m_bFirstTagAfterDocset	( false )
	, m_iKillListIterator		( 0 )
	, m_iMVA			( 0 )
	, m_iMVAIterator	( 0 )
	, m_iCurField		( -1 )
	, m_iCurAttr		( -1 )
	, m_pParser			( NULL )
	, m_iFieldBufferMax	( 65536 )
	, m_pFieldBuffer	( NULL )
	, m_iFieldBufferLen	( 0 )
	, m_bFixupUTF8		( false )
	, m_iReparseStart	( 0 )
	, m_iReparseLen		( 0 )
{
}


CSphSource_XMLPipe2::~CSphSource_XMLPipe2 ()
{
	Disconnect ();
	SafeDeleteArray ( m_pBuffer );
	SafeDeleteArray ( m_pFieldBuffer );
	ARRAY_FOREACH ( i, m_dParsedDocuments )
		SafeDelete ( m_dParsedDocuments[i] );
}


void CSphSource_XMLPipe2::Disconnect ()
{
	if ( m_pPipe )
	{
		pclose ( m_pPipe );
		m_pPipe = NULL;
	}

	if ( m_pParser )
	{
		XML_ParserFree ( m_pParser );
		m_pParser = NULL;
	}

	m_tHits.m_dData.Reset();
}


void CSphSource_XMLPipe2::Error ( const char * sTemplate, ... )
{
	if ( !m_sError.IsEmpty() )
		return;

	va_list ap;
	va_start ( ap, sTemplate );
	m_sError = DecorateMessageVA ( sTemplate, ap );
	va_end ( ap );
}


const char * CSphSource_XMLPipe2::DecorateMessage ( const char * sTemplate, ... ) const
{
	va_list ap;
	va_start ( ap, sTemplate );
	const char * sRes = DecorateMessageVA ( sTemplate, ap );
	va_end ( ap );
	return sRes;
}


const char * CSphSource_XMLPipe2::DecorateMessageVA ( const char * sTemplate, va_list ap ) const
{
	static char sBuf[1024];

	snprintf ( sBuf, sizeof(sBuf), "source '%s': ", m_tSchema.m_sName.cstr() );
	int iBufLen = strlen ( sBuf );
	int iLeft = sizeof(sBuf) - iBufLen;
	char * szBufStart = sBuf + iBufLen;

	vsnprintf ( szBufStart, iLeft, sTemplate, ap );
	iBufLen = strlen ( sBuf );
	iLeft = sizeof(sBuf) - iBufLen;
	szBufStart = sBuf + iBufLen;

	if ( m_pParser )
	{
		SphDocID_t uFailedID = 0;
		if ( m_dParsedDocuments.GetLength() )
			uFailedID = m_dParsedDocuments.Last()->m_iDocID;

		snprintf ( szBufStart, iLeft, " (line=%d, pos=%d, docid=" DOCID_FMT ")",
			(int)XML_GetCurrentLineNumber ( m_pParser ), (int)XML_GetCurrentColumnNumber ( m_pParser ),
			uFailedID );
	}

	return sBuf;
}


bool CSphSource_XMLPipe2::Setup ( int iFieldBufferMax, bool bFixupUTF8, FILE * pPipe, const CSphConfigSection & hSource )
{
	assert ( !m_pBuffer && !m_pFieldBuffer );

	m_pBuffer = new BYTE [m_iBufferSize];
	m_iFieldBufferMax = Max ( iFieldBufferMax, 65536 );
	m_pFieldBuffer = new BYTE [ m_iFieldBufferMax ];
	m_bFixupUTF8 = bFixupUTF8;
	m_pPipe = pPipe;
	m_tSchema.Reset ();
	bool bWordDict = ( m_pDict && m_pDict->GetSettings().m_bWordDict );

	ConfigureAttrs ( hSource("xmlpipe_attr_uint"),			SPH_ATTR_INTEGER,	m_tSchema );
	ConfigureAttrs ( hSource("xmlpipe_attr_timestamp"),		SPH_ATTR_TIMESTAMP,	m_tSchema );
	ConfigureAttrs ( hSource("xmlpipe_attr_str2ordinal"),	SPH_ATTR_ORDINAL,	m_tSchema );
	ConfigureAttrs ( hSource("xmlpipe_attr_bool"),			SPH_ATTR_BOOL,		m_tSchema );
	ConfigureAttrs ( hSource("xmlpipe_attr_float"),			SPH_ATTR_FLOAT,		m_tSchema );
	ConfigureAttrs ( hSource("xmlpipe_attr_bigint"),		SPH_ATTR_BIGINT,	m_tSchema );
	ConfigureAttrs ( hSource("xmlpipe_attr_multi"),			SPH_ATTR_UINT32SET,	m_tSchema );
	ConfigureAttrs ( hSource("xmlpipe_attr_multi_64"),		SPH_ATTR_INT64SET,	m_tSchema );
	ConfigureAttrs ( hSource("xmlpipe_attr_string"),		SPH_ATTR_STRING,	m_tSchema );
	ConfigureAttrs ( hSource("xmlpipe_attr_json"),			SPH_ATTR_JSON,		m_tSchema );
	ConfigureAttrs ( hSource("xmlpipe_attr_wordcount"),		SPH_ATTR_WORDCOUNT,	m_tSchema );

	ConfigureAttrs ( hSource("xmlpipe_field_string"),		SPH_ATTR_STRING,	m_tSchema );
	ConfigureAttrs ( hSource("xmlpipe_field_wordcount"),	SPH_ATTR_WORDCOUNT,	m_tSchema );

	ConfigureFields ( hSource("xmlpipe_field"), bWordDict, m_tSchema );
	ConfigureFields ( hSource("xmlpipe_field_string"), bWordDict, m_tSchema );
	ConfigureFields ( hSource("xmlpipe_field_wordcount"), bWordDict, m_tSchema );

	AllocDocinfo();
	return true;
}


bool CSphSource_XMLPipe2::Connect ( CSphString & sError )
{
	assert ( m_pBuffer && m_pFieldBuffer );

	ARRAY_FOREACH ( i, m_tSchema.m_dFields )
	{
		CSphColumnInfo & tCol = m_tSchema.m_dFields[i];
		tCol.m_eWordpart = GetWordpart ( tCol.m_sName.cstr(), m_pDict && m_pDict->GetSettings().m_bWordDict );
	}

	if ( !AddAutoAttrs ( sError ) )
		return false;
	AllocDocinfo();

	m_pParser = XML_ParserCreate(NULL);
	if ( !m_pParser )
	{
		sError.SetSprintf ( "xmlpipe: failed to create XML parser" );
		return false;
	}

	XML_SetUserData ( m_pParser, this );
	XML_SetElementHandler ( m_pParser, xmlStartElement, xmlEndElement );
	XML_SetCharacterDataHandler ( m_pParser, xmlCharacters );

#if USE_LIBICONV
	XML_SetUnknownEncodingHandler ( m_pParser, xmlUnknownEncoding, NULL );
#endif

	m_dKillList.Reserve ( 1024 );
	m_dKillList.Resize ( 0 );

	m_bRemoveParsed = false;
	m_bInDocset = false;
	m_bInSchema = false;
	m_bInDocument = false;
	m_bInKillList = false;
	m_bInId = false;
	m_bFirstTagAfterDocset = false;
	m_iCurField = -1;
	m_iCurAttr = -1;
	m_iElementDepth = 0;

	m_dParsedDocuments.Reset ();
	m_dDefaultAttrs.Reset ();
	m_dInvalid.Reset ();
	m_dWarned.Reset ();

	m_dParsedDocuments.Reserve ( 1024 );
	m_dParsedDocuments.Resize ( 0 );

	m_iKillListIterator = 0;

	m_iMVA = 0;
	m_iMVAIterator = 0;

	m_sError = "";

	int iBytesRead = fread ( m_pBuffer, 1, m_iBufferSize, m_pPipe );

	if ( !ParseNextChunk ( iBytesRead, sError ) )
		return false;

	m_dAttrToMVA.Resize ( 0 );

	int iFieldMVA = 0;
	for ( int i = 0; i < m_tSchema.GetAttrsCount (); i++ )
	{
		const CSphColumnInfo & tCol = m_tSchema.GetAttr ( i );
		if ( ( tCol.m_eAttrType==SPH_ATTR_UINT32SET || tCol.m_eAttrType==SPH_ATTR_INT64SET ) && tCol.m_eSrc==SPH_ATTRSRC_FIELD )
			m_dAttrToMVA.Add ( iFieldMVA++ );
		else
			m_dAttrToMVA.Add ( -1 );
	}

	m_dFieldMVAs.Resize ( iFieldMVA );
	ARRAY_FOREACH ( i, m_dFieldMVAs )
		m_dFieldMVAs[i].Reserve ( 16 );

	m_tHits.m_dData.Reserve ( m_iMaxHits );

	return true;
}


bool CSphSource_XMLPipe2::ParseNextChunk ( int iBufferLen, CSphString & sError )
{
	if ( !iBufferLen )
		return true;

	bool bLast = ( iBufferLen!=m_iBufferSize );

	m_iReparseLen = 0;
	if ( m_bFixupUTF8 )
	{
		BYTE * p = m_pBuffer;
		BYTE * pMax = m_pBuffer + iBufferLen;

		while ( p<pMax )
		{
			BYTE v = *p;

			// fix control codes
			if ( v<0x20 && v!=0x0D && v!=0x0A )
			{
				*p++ = ' ';
				continue;
			}

			// accept ascii7 codes
			if ( v<128 )
			{
				p++;
				continue;
			}

			// remove invalid start bytes
			if ( v<0xC2 )
			{
				*p++ = ' ';
				continue;
			}

			// get and check byte count
			int iBytes = 0;
			while ( v & 0x80 )
			{
				iBytes++;
				v <<= 1;
			}
			if ( iBytes<2 || iBytes>3 )
			{
				*p++ = ' ';
				continue;
			}

			// if we're on a boundary, save these few bytes for the future
			if ( p+iBytes>pMax )
			{
				m_iReparseStart = (int)(p-m_pBuffer);
				m_iReparseLen = (int)(pMax-p);
				iBufferLen -= m_iReparseLen;
				break;
			}

			// otherwise (not a boundary), check them all
			int i = 1;
			int iVal = ( v >> iBytes );
			for ( ; i<iBytes; i++ )
			{
				if ( ( p[i] & 0xC0 )!=0x80 )
					break;
				iVal = ( iVal<<6 ) + ( p[i] & 0x3f );
			}

			if ( i!=iBytes // remove invalid sequences
				|| ( iVal>=0xd800 && iVal<=0xdfff ) // and utf-16 surrogate pairs
				|| ( iBytes==3 && iVal<0x800 ) // and overlong 3-byte codes
				|| ( iVal>=0xfff0 && iVal<=0xffff ) ) // and kinda-valid specials expat chokes on anyway
			{
				iBytes = i;
				for ( i=0; i<iBytes; i++ )
					p[i] = ' ';
			}

			// only move forward by the amount of succesfully processed bytes!
			p += i;
		}
	}

	if ( XML_Parse ( m_pParser, (const char*) m_pBuffer, iBufferLen, bLast )!=XML_STATUS_OK )
	{
		SphDocID_t uFailedID = 0;
		if ( m_dParsedDocuments.GetLength() )
			uFailedID = m_dParsedDocuments.Last()->m_iDocID;

		sError.SetSprintf ( "source '%s': XML parse error: %s (line=%d, pos=%d, docid=" DOCID_FMT ")",
			m_tSchema.m_sName.cstr(), XML_ErrorString ( XML_GetErrorCode ( m_pParser ) ),
			(int)XML_GetCurrentLineNumber ( m_pParser ), (int)XML_GetCurrentColumnNumber ( m_pParser ),
			uFailedID );
		m_tDocInfo.m_iDocID = 1;
		return false;
	}

	if ( !m_sError.IsEmpty () )
	{
		sError = m_sError;
		m_tDocInfo.m_iDocID = 1;
		return false;
	}

	return true;
}


BYTE **	CSphSource_XMLPipe2::NextDocument ( CSphString & sError )
{
	assert ( m_pBuffer && m_pFieldBuffer );

	if ( m_bRemoveParsed )
	{
		SafeDelete ( m_dParsedDocuments[0] );
		m_dParsedDocuments.RemoveFast ( 0 );
		m_bRemoveParsed = false;
	}

	int iReadResult = 0;

	while ( m_dParsedDocuments.GetLength()==0 )
	{
		// saved bytes to the front!
		if ( m_iReparseLen )
			memmove ( m_pBuffer, m_pBuffer+m_iReparseStart, m_iReparseLen );

		// read more data
		iReadResult = fread ( m_pBuffer+m_iReparseLen, 1, m_iBufferSize-m_iReparseLen, m_pPipe );
		if ( iReadResult==0 )
			break;

		// and parse it
		if ( !ParseNextChunk ( iReadResult+m_iReparseLen, sError ) )
			return NULL;
	}

	while ( m_dParsedDocuments.GetLength()!=0 )
	{
		Document_t * pDocument = m_dParsedDocuments[0];
		int nAttrs = m_tSchema.GetAttrsCount ();

		// docid
		m_tDocInfo.m_iDocID = VerifyID ( pDocument->m_iDocID );
		if ( m_tDocInfo.m_iDocID==0 )
		{
			SafeDelete ( m_dParsedDocuments[0] );
			m_dParsedDocuments.RemoveFast ( 0 );
			continue;
		}

		int iFieldLenAttr = nAttrs;
		if ( m_bIndexFieldLens )
			iFieldLenAttr = nAttrs - m_tSchema.m_dFields.GetLength();

		// attributes
		for ( int i = 0; i < nAttrs; i++ )
		{
			const CSphColumnInfo & tAttr = m_tSchema.GetAttr ( i );

			// reset, and the value will be filled by IterateHits()
			if ( i>=iFieldLenAttr )
			{
				assert ( tAttr.m_eAttrType==SPH_ATTR_TOKENCOUNT );
				m_tDocInfo.SetAttr ( tAttr.m_tLocator, 0 );
				continue;
			}

			const CSphString & sAttrValue = pDocument->m_dAttrs[i].IsEmpty () && m_dDefaultAttrs.GetLength ()
				? m_dDefaultAttrs[i]
				: pDocument->m_dAttrs[i];

			if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET || tAttr.m_eAttrType==SPH_ATTR_INT64SET )
			{
				m_tDocInfo.SetAttr ( tAttr.m_tLocator, ParseFieldMVA ( m_dMva, sAttrValue.cstr (), tAttr.m_eAttrType==SPH_ATTR_INT64SET ) );
				continue;
			}

			switch ( tAttr.m_eAttrType )
			{
				case SPH_ATTR_ORDINAL:
				case SPH_ATTR_STRING:
				case SPH_ATTR_JSON:
				case SPH_ATTR_WORDCOUNT:
					m_dStrAttrs[i] = sAttrValue.cstr ();
					if ( !m_dStrAttrs[i].cstr() )
						m_dStrAttrs[i] = "";

					m_tDocInfo.SetAttr ( tAttr.m_tLocator, 0 );
					break;

				case SPH_ATTR_FLOAT:
					m_tDocInfo.SetAttrFloat ( tAttr.m_tLocator, sphToFloat ( sAttrValue.cstr () ) );
					break;

				case SPH_ATTR_BIGINT:
					m_tDocInfo.SetAttr ( tAttr.m_tLocator, sphToInt64 ( sAttrValue.cstr () ) );
					break;

				default:
					m_tDocInfo.SetAttr ( tAttr.m_tLocator, sphToDword ( sAttrValue.cstr () ) );
					break;
			}
		}

		m_bRemoveParsed = true;

		int nFields = m_tSchema.m_dFields.GetLength ();
		if ( !nFields )
		{
			m_tDocInfo.m_iDocID = 0;
			return NULL;
		}

		m_dFieldPtrs.Resize ( nFields );
		for ( int i = 0; i < nFields; ++i )
			m_dFieldPtrs[i] = pDocument->m_dFields[i].Begin();

		return (BYTE **)&( m_dFieldPtrs[0] );
	}

	if ( !iReadResult )
		m_tDocInfo.m_iDocID = 0;

	return NULL;
}


bool CSphSource_XMLPipe2::IterateKillListStart ( CSphString & )
{
	m_iKillListIterator = 0;
	return true;
}


bool CSphSource_XMLPipe2::IterateKillListNext ( SphDocID_t & tDocId )
{
	if ( m_iKillListIterator>=m_dKillList.GetLength () )
		return false;

	tDocId = m_dKillList [ m_iKillListIterator++ ];
	return true;
}

enum EXMLElem
{
	ELEM_DOCSET,
	ELEM_SCHEMA,
	ELEM_FIELD,
	ELEM_ATTR,
	ELEM_DOCUMENT,
	ELEM_KLIST,
	ELEM_NONE
};

static EXMLElem LookupElement ( const char * szName )
{
	if ( szName[0]!='s' )
		return ELEM_NONE;

	int iLen = strlen(szName);
	if ( iLen>=11 && iLen<=15 )
	{
		char iHash = (char)( ( iLen + szName[7] ) & 15 );
		switch ( iHash )
		{
		case 1:		if ( !strcmp ( szName, "sphinx:docset" ) )		return ELEM_DOCSET;
		case 0:		if ( !strcmp ( szName, "sphinx:schema" ) )		return ELEM_SCHEMA;
		case 2:		if ( !strcmp ( szName, "sphinx:field" ) )		return ELEM_FIELD;
		case 12:	if ( !strcmp ( szName, "sphinx:attr" ) )		return ELEM_ATTR;
		case 3:		if ( !strcmp ( szName, "sphinx:document" ) )	return ELEM_DOCUMENT;
		case 10:	if ( !strcmp ( szName, "sphinx:killlist" ) )	return ELEM_KLIST;
		}
	}

	return ELEM_NONE;
}

void CSphSource_XMLPipe2::StartElement ( const char * szName, const char ** pAttrs )
{
	EXMLElem ePos = LookupElement ( szName );

	switch ( ePos )
	{
	case ELEM_DOCSET:
		m_bInDocset = true;
		m_bFirstTagAfterDocset = true;
		return;

	case ELEM_SCHEMA:
	{
		if ( !m_bInDocset || !m_bFirstTagAfterDocset )
		{
			Error ( "<sphinx:schema> is allowed immediately after <sphinx:docset> only" );
			return;
		}

		if ( m_tSchema.m_dFields.GetLength () > 0 || m_tSchema.GetAttrsCount () > 0 )
		{
			sphWarn ( "%s", DecorateMessage ( "both embedded and configured schemas found; using embedded" ) );
			m_tSchema.Reset ();
			CSphMatch tDocInfo;
			Swap ( m_tDocInfo, tDocInfo );
		}

		m_bFirstTagAfterDocset = false;
		m_bInSchema = true;
	}
	return;

	case ELEM_FIELD:
	{
		if ( !m_bInDocset || !m_bInSchema )
		{
			Error ( "<sphinx:field> is allowed inside <sphinx:schema> only" );
			return;
		}

		const char ** dAttrs = pAttrs;
		CSphColumnInfo Info;
		CSphString sDefault;
		bool bIsAttr = false;
		bool bWordDict = ( m_pDict && m_pDict->GetSettings().m_bWordDict );

		while ( dAttrs[0] && dAttrs[1] && dAttrs[0][0] && dAttrs[1][0] )
		{
			if ( !strcmp ( *dAttrs, "name" ) )
			{
				AddFieldToSchema ( dAttrs[1], bWordDict, m_tSchema );
				Info.m_sName = dAttrs[1];
			} else if ( !strcmp ( *dAttrs, "attr" ) )
			{
				bIsAttr = true;
				if ( !strcmp ( dAttrs[1], "string" ) )
					Info.m_eAttrType = SPH_ATTR_STRING;
				else if ( !strcmp ( dAttrs[1], "json" ) )
					Info.m_eAttrType = SPH_ATTR_JSON;
				else if ( !strcmp ( dAttrs[1], "wordcount" ) )
					Info.m_eAttrType = SPH_ATTR_WORDCOUNT;

			} else if ( !strcmp ( *dAttrs, "default" ) )
				sDefault = dAttrs[1];

			dAttrs += 2;
		}

		if ( bIsAttr )
		{
			Info.m_iIndex = m_tSchema.GetAttrsCount ();
			m_tSchema.AddAttr ( Info, true ); // all attributes are dynamic at indexing time
			m_dDefaultAttrs.Add ( sDefault );
		}
	}
	return;

	case ELEM_ATTR:
	{
		if ( !m_bInDocset || !m_bInSchema )
		{
			Error ( "<sphinx:attr> is allowed inside <sphinx:schema> only" );
			return;
		}

		bool bError = false;
		CSphString sDefault;

		CSphColumnInfo Info;
		Info.m_eAttrType = SPH_ATTR_INTEGER;

		const char ** dAttrs = pAttrs;

		while ( dAttrs[0] && dAttrs[1] && dAttrs[0][0] && dAttrs[1][0] && !bError )
		{
			if ( !strcmp ( *dAttrs, "name" ) )
				Info.m_sName = dAttrs[1];
			else if ( !strcmp ( *dAttrs, "bits" ) )
				Info.m_tLocator.m_iBitCount = strtol ( dAttrs[1], NULL, 10 );
			else if ( !strcmp ( *dAttrs, "default" ) )
				sDefault = dAttrs[1];
			else if ( !strcmp ( *dAttrs, "type" ) )
			{
				const char * szType = dAttrs[1];
				if ( !strcmp ( szType, "int" ) )				Info.m_eAttrType = SPH_ATTR_INTEGER;
				else if ( !strcmp ( szType, "timestamp" ) )		Info.m_eAttrType = SPH_ATTR_TIMESTAMP;
				else if ( !strcmp ( szType, "str2ordinal" ) )	Info.m_eAttrType = SPH_ATTR_ORDINAL;
				else if ( !strcmp ( szType, "bool" ) )			Info.m_eAttrType = SPH_ATTR_BOOL;
				else if ( !strcmp ( szType, "float" ) )			Info.m_eAttrType = SPH_ATTR_FLOAT;
				else if ( !strcmp ( szType, "bigint" ) )		Info.m_eAttrType = SPH_ATTR_BIGINT;
				else if ( !strcmp ( szType, "string" ) )		Info.m_eAttrType = SPH_ATTR_STRING;
				else if ( !strcmp ( szType, "json" ) )			Info.m_eAttrType = SPH_ATTR_JSON;
				else if ( !strcmp ( szType, "wordcount" ) )		Info.m_eAttrType = SPH_ATTR_WORDCOUNT;
				else if ( !strcmp ( szType, "multi" ) )
				{
					Info.m_eAttrType = SPH_ATTR_UINT32SET;
					Info.m_eSrc = SPH_ATTRSRC_FIELD;
				} else if ( !strcmp ( szType, "multi_64" ) )
				{
					Info.m_eAttrType = SPH_ATTR_INT64SET;
					Info.m_eSrc = SPH_ATTRSRC_FIELD;
				} else
				{
					Error ( "unknown column type '%s'", szType );
					bError = true;
				}
			}

			dAttrs += 2;
		}

		if ( !bError )
		{
			Info.m_iIndex = m_tSchema.GetAttrsCount ();
			m_tSchema.AddAttr ( Info, true ); // all attributes are dynamic at indexing time
			m_dDefaultAttrs.Add ( sDefault );
		}
	}
	return;

	case ELEM_DOCUMENT:
	{
		if ( !m_bInDocset || m_bInSchema )
			return DocumentError ( "<sphinx:schema>" );

		if ( m_bInKillList )
			return DocumentError ( "<sphinx:killlist>" );

		if ( m_bInDocument )
			return DocumentError ( "<sphinx:document>" );

		if ( m_tSchema.m_dFields.GetLength()==0 && m_tSchema.GetAttrsCount()==0 )
		{
			Error ( "no schema configured, and no embedded schema found" );
			return;
		}

		m_bInDocument = true;

		assert ( !m_pCurDocument );
		m_pCurDocument = new Document_t;

		m_pCurDocument->m_iDocID = 0;
		m_pCurDocument->m_dFields.Resize ( m_tSchema.m_dFields.GetLength () );
		// for safety
		ARRAY_FOREACH ( i, m_pCurDocument->m_dFields )
			m_pCurDocument->m_dFields[i].Add ( '\0' );
		m_pCurDocument->m_dAttrs.Resize ( m_tSchema.GetAttrsCount () );

		if ( pAttrs[0] && pAttrs[1] && pAttrs[0][0] && pAttrs[1][0] )
			if ( !strcmp ( pAttrs[0], "id" ) )
				m_pCurDocument->m_iDocID = sphToDocid ( pAttrs[1] );

		if ( m_pCurDocument->m_iDocID==0 )
			Error ( "attribute 'id' required in <sphinx:document>" );
	}
	return;

	case ELEM_KLIST:
	{
		if ( !m_bInDocset || m_bInDocument || m_bInSchema )
		{
			Error ( "<sphinx:killlist> is not allowed inside <sphinx:schema> or <sphinx:document>" );
			return;
		}

		m_bInKillList = true;
	}
	return;

	case ELEM_NONE: break; // avoid warning
	}

	if ( m_bInKillList )
	{
		if ( m_bInId )
		{
			m_iElementDepth++;
			return;
		}

		if ( strcmp ( szName, "id" ) )
		{
			Error ( "only 'id' is allowed inside <sphinx:killlist>" );
			return;
		}

		m_bInId = true;

	} else if ( m_bInDocument )
	{
		if ( m_iCurField!=-1 || m_iCurAttr!=-1 )
		{
			m_iElementDepth++;
			return;
		}

		for ( int i = 0; i < m_tSchema.m_dFields.GetLength () && m_iCurField==-1; i++ )
			if ( m_tSchema.m_dFields[i].m_sName==szName )
				m_iCurField = i;

		m_iCurAttr = m_tSchema.GetAttrIndex ( szName );

		if ( m_iCurAttr!=-1 || m_iCurField!=-1 )
			return;

		m_bInIgnoredTag = true;

		bool bInvalidFound = false;
		for ( int i = 0; i < m_dInvalid.GetLength () && !bInvalidFound; i++ )
			bInvalidFound = m_dInvalid[i]==szName;

		if ( !bInvalidFound )
		{
			sphWarn ( "%s", DecorateMessage ( "unknown field/attribute '%s'; ignored", szName ) );
			m_dInvalid.Add ( szName );
		}
	}
}


void CSphSource_XMLPipe2::EndElement ( const char * szName )
{
	m_bInIgnoredTag = false;

	EXMLElem ePos = LookupElement ( szName );

	switch ( ePos )
	{
	case ELEM_DOCSET:
		m_bInDocset = false;
		return;

	case ELEM_SCHEMA:
		m_bInSchema = false;
		AddAutoAttrs ( m_sError );
		AllocDocinfo();
		return;

	case ELEM_DOCUMENT:
		m_bInDocument = false;
		if ( m_pCurDocument )
			m_dParsedDocuments.Add ( m_pCurDocument );
		m_pCurDocument = NULL;
		return;

	case ELEM_KLIST:
		m_bInKillList = false;
		return;

	case ELEM_FIELD: // avoid warnings
	case ELEM_ATTR:
	case ELEM_NONE: break;
	}

	if ( m_bInKillList )
	{
		if ( m_iElementDepth!=0 )
		{
			m_iElementDepth--;
			return;
		}

		if ( m_bInId )
		{
			m_pFieldBuffer [ Min ( m_iFieldBufferLen, m_iFieldBufferMax-1 ) ] = '\0';
			m_dKillList.Add ( sphToDocid ( (const char *)m_pFieldBuffer ) );
			m_iFieldBufferLen = 0;
			m_bInId = false;
		}

	} else if ( m_bInDocument && ( m_iCurAttr!=-1 || m_iCurField!=-1 ) )
	{
		if ( m_iElementDepth!=0 )
		{
			m_iElementDepth--;
			return;
		}

		if ( m_iCurField!=-1 )
		{
			assert ( m_pCurDocument );
			CSphVector<BYTE> & dBuf = m_pCurDocument->m_dFields [ m_iCurField ];

			dBuf.Last() = ' ';
			dBuf.Reserve ( dBuf.GetLength() + m_iFieldBufferLen + 6 ); // 6 is a safety gap
			memcpy ( dBuf.Begin()+dBuf.GetLength(), m_pFieldBuffer, m_iFieldBufferLen );
			dBuf.Resize ( dBuf.GetLength()+m_iFieldBufferLen );
			dBuf.Add ( '\0' );
		}
		if ( m_iCurAttr!=-1 )
		{
			assert ( m_pCurDocument );
			if ( !m_pCurDocument->m_dAttrs [ m_iCurAttr ].IsEmpty () )
				sphWarn ( "duplicate attribute node <%s> - using first value", m_tSchema.GetAttr ( m_iCurAttr ).m_sName.cstr() );
			else
				m_pCurDocument->m_dAttrs [ m_iCurAttr ].SetBinary ( (char*)m_pFieldBuffer, m_iFieldBufferLen );
		}

		m_iFieldBufferLen = 0;

		m_iCurAttr = -1;
		m_iCurField = -1;
	}
}


void CSphSource_XMLPipe2::UnexpectedCharaters ( const char * pCharacters, int iLen, const char * szComment )
{
	const int MAX_WARNING_LENGTH = 64;

	bool bSpaces = true;
	for ( int i = 0; i < iLen && bSpaces; i++ )
		if ( !sphIsSpace ( pCharacters[i] ) )
			bSpaces = false;

	if ( !bSpaces )
	{
		CSphString sWarning;
		sWarning.SetBinary ( pCharacters, Min ( iLen, MAX_WARNING_LENGTH ) );
		sphWarn ( "source '%s': unexpected string '%s' (line=%d, pos=%d) %s",
			m_tSchema.m_sName.cstr(), sWarning.cstr (),
			(int)XML_GetCurrentLineNumber ( m_pParser ), (int)XML_GetCurrentColumnNumber ( m_pParser ), szComment );
	}
}


void CSphSource_XMLPipe2::Characters ( const char * pCharacters, int iLen )
{
	if ( m_bInIgnoredTag )
		return;

	if ( !m_bInDocset )
	{
		UnexpectedCharaters ( pCharacters, iLen, "outside of <sphinx:docset>" );
		return;
	}

	if ( !m_bInSchema && !m_bInDocument && !m_bInKillList )
	{
		UnexpectedCharaters ( pCharacters, iLen, "outside of <sphinx:schema> and <sphinx:document>" );
		return;
	}

	if ( m_iCurAttr==-1 && m_iCurField==-1 && !m_bInKillList )
	{
		UnexpectedCharaters ( pCharacters, iLen, m_bInDocument ? "inside <sphinx:document>" : ( m_bInSchema ? "inside <sphinx:schema>" : "" ) );
		return;
	}

	if ( iLen + m_iFieldBufferLen < m_iFieldBufferMax )
	{
		memcpy ( m_pFieldBuffer + m_iFieldBufferLen, pCharacters, iLen );
		m_iFieldBufferLen += iLen;

	} else
	{
		const CSphString & sName = ( m_iCurField!=-1 ) ? m_tSchema.m_dFields[m_iCurField].m_sName : m_tSchema.GetAttr ( m_iCurAttr ).m_sName;

		bool bWarned = false;
		for ( int i = 0; i < m_dWarned.GetLength () && !bWarned; i++ )
			bWarned = m_dWarned[i]==sName;

		if ( !bWarned )
		{
			sphWarn ( "source '%s': field/attribute '%s' length exceeds max length (line=%d, pos=%d, docid=" DOCID_FMT ")",
				m_tSchema.m_sName.cstr(), sName.cstr(),
				(int)XML_GetCurrentLineNumber ( m_pParser ), (int)XML_GetCurrentColumnNumber ( m_pParser ),
				m_pCurDocument->m_iDocID );

			m_dWarned.Add ( sName );
		}
	}
}

CSphSource * sphCreateSourceXmlpipe2 ( const CSphConfigSection * pSource, FILE * pPipe,
	const char * szSourceName, int iMaxFieldLen, bool RLPARG(bProxy) )
{
	CSphSource_XMLPipe2 * pXMLPipe;
	bool bUTF8 = pSource->GetInt ( "xmlpipe_fixup_utf8", 0 )!=0;

#if USE_RLP
	if ( bProxy )
		pXMLPipe = new CSphSource_Proxy<CSphSource_XMLPipe2> ( szSourceName );
	else
#endif
		pXMLPipe = new CSphSource_XMLPipe2 ( szSourceName );

	if ( !pXMLPipe->Setup ( iMaxFieldLen, bUTF8, pPipe, *pSource ) )
		SafeDelete ( pXMLPipe );

	return pXMLPipe;
}

#endif


#if USE_ODBC

CSphSourceParams_ODBC::CSphSourceParams_ODBC ()
	: m_bWinAuth	( false )
	, m_bUnicode	( false )
{
}


CSphSource_ODBC::CSphSource_ODBC ( const char * sName )
	: CSphSource_SQL	( sName )
	, m_bWinAuth		( false )
	, m_bUnicode		( false )
	, m_hEnv			( NULL )
	, m_hDBC			( NULL )
	, m_hStmt			( NULL )
	, m_nResultCols		( 0 )
{
}


void CSphSource_ODBC::SqlDismissResult ()
{
	if ( m_hStmt )
	{
		SQLCloseCursor ( m_hStmt );
		SQLFreeHandle ( SQL_HANDLE_STMT, m_hStmt );
		m_hStmt = NULL;
	}
}


#define MS_SQL_BUFFER_GAP 16


bool CSphSource_ODBC::SqlQuery ( const char * sQuery )
{
	if ( SQLAllocHandle ( SQL_HANDLE_STMT, m_hDBC, &m_hStmt )==SQL_ERROR )
	{
		if ( m_tParams.m_bPrintQueries )
			fprintf ( stdout, "SQL-QUERY: %s: FAIL (SQLAllocHandle failed)\n", sQuery );
		return false;
	}

	if ( SQLExecDirect ( m_hStmt, (SQLCHAR *)sQuery, SQL_NTS )==SQL_ERROR )
	{
		GetSqlError ( SQL_HANDLE_STMT, m_hStmt );
		if ( m_tParams.m_bPrintQueries )
			fprintf ( stdout, "SQL-QUERY: %s: FAIL\n", sQuery );
		return false;
	}
	if ( m_tParams.m_bPrintQueries )
		fprintf ( stdout, "SQL-QUERY: %s: ok\n", sQuery );

	SQLSMALLINT nCols = 0;
	m_nResultCols = 0;
	if ( SQLNumResultCols ( m_hStmt, &nCols )==SQL_ERROR )
		return false;

	m_nResultCols = nCols;

	const int MAX_NAME_LEN = 512;
	char szColumnName[MAX_NAME_LEN];

	m_dColumns.Resize ( m_nResultCols );
	int iTotalBuffer = 0;
	ARRAY_FOREACH ( i, m_dColumns )
	{
		QueryColumn_t & tCol = m_dColumns[i];

		SQLULEN uColSize = 0;
		SQLSMALLINT iNameLen = 0;
		SQLSMALLINT iDataType = 0;
		if ( SQLDescribeCol ( m_hStmt, (SQLUSMALLINT)(i+1), (SQLCHAR*)szColumnName,
			MAX_NAME_LEN, &iNameLen, &iDataType, &uColSize, NULL, NULL )==SQL_ERROR )
				return false;

		tCol.m_sName = szColumnName;
		tCol.m_sName.ToLower();

		// deduce buffer size
		// use a small buffer by default, and a bigger one for varchars
		int iBuffLen = DEFAULT_COL_SIZE;
		if ( iDataType==SQL_WCHAR || iDataType==SQL_WVARCHAR || iDataType==SQL_WLONGVARCHAR|| iDataType==SQL_VARCHAR )
			iBuffLen = VARCHAR_COL_SIZE;

		if ( m_hColBuffers ( tCol.m_sName ) )
			iBuffLen = m_hColBuffers [ tCol.m_sName ]; // got explicit user override
		else if ( uColSize )
			iBuffLen = Min ( uColSize+1, (SQLULEN) MAX_COL_SIZE ); // got data from driver

		tCol.m_dContents.Resize ( iBuffLen + MS_SQL_BUFFER_GAP );
		tCol.m_dRaw.Resize ( iBuffLen + MS_SQL_BUFFER_GAP );
		tCol.m_iInd = 0;
		tCol.m_iBufferSize = iBuffLen;
		tCol.m_bUnicode = m_bUnicode && ( iDataType==SQL_WCHAR || iDataType==SQL_WVARCHAR || iDataType==SQL_WLONGVARCHAR );
		tCol.m_bTruncated = false;
		iTotalBuffer += iBuffLen;

		if ( SQLBindCol ( m_hStmt, (SQLUSMALLINT)(i+1),
			tCol.m_bUnicode ? SQL_UNICODE : SQL_C_CHAR,
			tCol.m_bUnicode ? tCol.m_dRaw.Begin() : tCol.m_dContents.Begin(),
			iBuffLen, &(tCol.m_iInd) )==SQL_ERROR )
				return false;
	}

	if ( iTotalBuffer>WARN_ROW_SIZE )
		sphWarn ( "row buffer is over %d bytes; consider revising sql_column_buffers", iTotalBuffer );

	return true;
}


bool CSphSource_ODBC::SqlIsError ()
{
	return !m_sError.IsEmpty ();
}


const char * CSphSource_ODBC::SqlError ()
{
	return m_sError.cstr();
}


bool CSphSource_ODBC::SqlConnect ()
{
	if ( SQLAllocHandle ( SQL_HANDLE_ENV, NULL, &m_hEnv )==SQL_ERROR )
	{
		if ( m_tParams.m_bPrintQueries )
			fprintf ( stdout, "SQL-CONNECT: FAIL\n" );
		return false;
	}

	SQLSetEnvAttr ( m_hEnv, SQL_ATTR_ODBC_VERSION, (void*) SQL_OV_ODBC3, SQL_IS_INTEGER );

	if ( SQLAllocHandle ( SQL_HANDLE_DBC, m_hEnv, &m_hDBC )==SQL_ERROR )
	{
		if ( m_tParams.m_bPrintQueries )
			fprintf ( stdout, "SQL-CONNECT: FAIL\n" );
		return false;
	}

	OdbcPostConnect ();

	char szOutConn [2048];
	SQLSMALLINT iOutConn = 0;
	if ( SQLDriverConnect ( m_hDBC, NULL, (SQLTCHAR*) m_sOdbcDSN.cstr(), SQL_NTS,
		(SQLCHAR*)szOutConn, sizeof(szOutConn), &iOutConn, SQL_DRIVER_NOPROMPT )==SQL_ERROR )
	{
		GetSqlError ( SQL_HANDLE_DBC, m_hDBC );
		if ( m_tParams.m_bPrintQueries )
			fprintf ( stdout, "SQL-CONNECT: FAIL\n" );
		return false;
	}

	if ( m_tParams.m_bPrintQueries )
		fprintf ( stdout, "SQL-CONNECT: ok\n" );
	return true;
}


void CSphSource_ODBC::SqlDisconnect ()
{
	if ( m_tParams.m_bPrintQueries )
		fprintf ( stdout, "SQL-DISCONNECT\n" );

	if ( m_hStmt!=NULL )
		SQLFreeHandle ( SQL_HANDLE_STMT, m_hStmt );

	if ( m_hDBC )
	{
		SQLDisconnect ( m_hDBC );
		SQLFreeHandle ( SQL_HANDLE_DBC, m_hDBC );
	}

	if ( m_hEnv )
		SQLFreeHandle ( SQL_HANDLE_ENV, m_hEnv );
}


int CSphSource_ODBC::SqlNumFields ()
{
	if ( !m_hStmt )
		return -1;

	return m_nResultCols;
}


bool CSphSource_ODBC::SqlFetchRow ()
{
	if ( !m_hStmt )
		return false;

	SQLRETURN iRet = SQLFetch ( m_hStmt );
	if ( iRet==SQL_ERROR || iRet==SQL_INVALID_HANDLE || iRet==SQL_NO_DATA )
	{
		GetSqlError ( SQL_HANDLE_STMT, m_hStmt );
		return false;
	}

	ARRAY_FOREACH ( i, m_dColumns )
	{
		QueryColumn_t & tCol = m_dColumns[i];
		switch ( tCol.m_iInd )
		{
			case SQL_NULL_DATA:
				tCol.m_dContents[0] = '\0';
				break;

			default:
#if USE_WINDOWS // FIXME! support UCS-2 columns on Unix too
				if ( tCol.m_bUnicode )
				{
					// WideCharToMultiByte should get NULL terminated string
					memset ( tCol.m_dRaw.Begin()+tCol.m_iBufferSize, 0, MS_SQL_BUFFER_GAP );

					int iConv = WideCharToMultiByte ( CP_UTF8, 0, LPCWSTR ( tCol.m_dRaw.Begin() ), tCol.m_iInd/sizeof(WCHAR),
						LPSTR ( tCol.m_dContents.Begin() ), tCol.m_iBufferSize-1, NULL, NULL );

					if ( iConv==0 )
						if ( GetLastError()==ERROR_INSUFFICIENT_BUFFER )
							iConv = tCol.m_iBufferSize-1;

					tCol.m_dContents[iConv] = '\0';

				} else
#endif
				{
					if ( tCol.m_iInd>=0 && tCol.m_iInd<tCol.m_iBufferSize )
					{
						// data fetched ok; add trailing zero
						tCol.m_dContents[tCol.m_iInd] = '\0';

					} else if ( tCol.m_iInd>=tCol.m_iBufferSize && !tCol.m_bTruncated )
					{
						// out of buffer; warn about that (once)
						tCol.m_bTruncated = true;
						sphWarn ( "'%s' column truncated (buffer=%d, got=%d); consider revising sql_column_buffers",
							tCol.m_sName.cstr(), tCol.m_iBufferSize-1, (int) tCol.m_iInd );
					}
				}
			break;
		}
	}

	return iRet!=SQL_NO_DATA;
}


const char * CSphSource_ODBC::SqlColumn ( int iIndex )
{
	if ( !m_hStmt )
		return NULL;

	return &(m_dColumns [iIndex].m_dContents[0]);
}


const char * CSphSource_ODBC::SqlFieldName ( int iIndex )
{
	return m_dColumns[iIndex].m_sName.cstr();
}


DWORD CSphSource_ODBC::SqlColumnLength ( int )
{
	return 0;
}


bool CSphSource_ODBC::Setup ( const CSphSourceParams_ODBC & tParams )
{
	if ( !CSphSource_SQL::Setup ( tParams ) )
		return false;

	// parse column buffers spec, if any
	if ( !tParams.m_sColBuffers.IsEmpty() )
	{
		const char * p = tParams.m_sColBuffers.cstr();
		while ( *p )
		{
			// skip space
			while ( sphIsSpace(*p) )
				p++;

			// expect eof or ident
			if ( !*p )
				break;
			if ( !sphIsAlpha(*p) )
			{
				m_sError.SetSprintf ( "identifier expected in sql_column_buffers near '%s'", p );
				return false;
			}

			// get ident
			CSphString sCol;
			const char * pIdent = p;
			while ( sphIsAlpha(*p) )
				p++;
			sCol.SetBinary ( pIdent, p-pIdent );

			// skip space
			while ( sphIsSpace(*p) )
				p++;

			// expect assignment
			if ( *p!='=' )
			{
				m_sError.SetSprintf ( "'=' expected in sql_column_buffers near '%s'", p );
				return false;
			}
			p++;

			// skip space
			while ( sphIsSpace(*p) )
				p++;

			// expect number
			if (!( *p>='0' && *p<='9' ))
			{
				m_sError.SetSprintf ( "number expected in sql_column_buffers near '%s'", p );
				return false;
			}

			// get value
			int iSize = 0;
			while ( *p>='0' && *p<='9' )
			{
				iSize = 10*iSize + ( *p-'0' );
				p++;
			}
			if ( *p=='K' )
			{
				iSize *= 1024;
				p++;
			} else if ( *p=='M' )
			{
				iSize *= 1048576;
				p++;
			}

			// hash value
			sCol.ToLower();
			m_hColBuffers.Add ( iSize, sCol );

			// skip space
			while ( sphIsSpace(*p) )
				p++;

			// expect eof or comma
			if ( !*p )
				break;
			if ( *p!=',' )
			{
				m_sError.SetSprintf ( "comma expected in sql_column_buffers near '%s'", p );
				return false;
			}
			p++;
		}
	}

	// ODBC specific params
	m_sOdbcDSN = tParams.m_sOdbcDSN;
	m_bWinAuth = tParams.m_bWinAuth;
	m_bUnicode = tParams.m_bUnicode;

	// build and store DSN for error reporting
	char sBuf [ 1024 ];
	snprintf ( sBuf, sizeof(sBuf), "odbc%s", m_sSqlDSN.cstr()+3 );
	m_sSqlDSN = sBuf;

	return true;
}


void CSphSource_ODBC::GetSqlError ( SQLSMALLINT iHandleType, SQLHANDLE hHandle )
{
	if ( !hHandle )
	{
		m_sError.SetSprintf ( "invalid handle" );
		return;
	}

	char szState[16] = "";
	char szMessageText[1024] = "";
	SQLINTEGER iError;
	SQLSMALLINT iLen;
	SQLGetDiagRec ( iHandleType, hHandle, 1, (SQLCHAR*)szState, &iError, (SQLCHAR*)szMessageText, 1024, &iLen );
	m_sError = szMessageText;
}

//////////////////////////////////////////////////////////////////////////

void CSphSource_MSSQL::OdbcPostConnect ()
{
	const int MAX_LEN = 1024;
	char szDriver[MAX_LEN];
	char szDriverAttrs[MAX_LEN];
	SQLSMALLINT iDescLen = 0;
	SQLSMALLINT iAttrLen = 0;
	SQLSMALLINT iDir = SQL_FETCH_FIRST;

	CSphString sDriver;
	for ( ;; )
	{
		SQLRETURN iRet = SQLDrivers ( m_hEnv, iDir, (SQLCHAR*)szDriver, MAX_LEN, &iDescLen, (SQLCHAR*)szDriverAttrs, MAX_LEN, &iAttrLen );
		if ( iRet==SQL_NO_DATA )
			break;

		iDir = SQL_FETCH_NEXT;
		if ( !strcmp ( szDriver, "SQL Native Client" )
			|| !strncmp ( szDriver, "SQL Server Native Client", strlen("SQL Server Native Client") ) )
		{
			sDriver = szDriver;
			break;
		}
	}

	if ( sDriver.IsEmpty() )
		sDriver = "SQL Server";

	if ( m_sOdbcDSN.IsEmpty() )
	{
		if ( m_bWinAuth && m_tParams.m_sUser.IsEmpty () )
		{
			m_sOdbcDSN.SetSprintf ( "DRIVER={%s};SERVER={%s};Database={%s};Trusted_Connection=yes",
				sDriver.cstr (), m_tParams.m_sHost.cstr (), m_tParams.m_sDB.cstr () );

		} else if ( m_bWinAuth )
		{
			m_sOdbcDSN.SetSprintf ( "DRIVER={%s};SERVER={%s};UID={%s};PWD={%s};Database={%s};Trusted_Connection=yes",
				sDriver.cstr (), m_tParams.m_sHost.cstr (), m_tParams.m_sUser.cstr (), m_tParams.m_sPass.cstr (), m_tParams.m_sDB.cstr () );
		} else
		{
			m_sOdbcDSN.SetSprintf ( "DRIVER={%s};SERVER={%s};UID={%s};PWD={%s};Database={%s}",
				sDriver.cstr (), m_tParams.m_sHost.cstr (), m_tParams.m_sUser.cstr (), m_tParams.m_sPass.cstr (), m_tParams.m_sDB.cstr () );
		}
	}
}

#endif


struct RemapXSV_t
{
	int m_iAttr;
	int m_iField;
};


class CSphSource_BaseSV : public CSphSource_Document, public CSphSchemaConfigurator<CSphSource_BaseSV>
{
public:
	explicit			CSphSource_BaseSV ( const char * sName );
	~CSphSource_BaseSV ();

	virtual bool	Connect ( CSphString & sError );				///< run the command and open the pipe
	virtual void	Disconnect ();									///< close the pipe
	const char *	DecorateMessage ( const char * sTemplate, ... ) const __attribute__ ( ( format ( printf, 2, 3 ) ) );

	virtual bool	IterateStart ( CSphString & );					///< Connect() starts getting documents automatically, so this one is empty
	virtual BYTE **	NextDocument ( CSphString & );					///< parse incoming chunk and emit some hits

	virtual bool	HasAttrsConfigured ()							{ return ( m_tSchema.GetAttrsCount()>0 ); }
	virtual bool	IterateMultivaluedStart ( int, CSphString & )	{ return false; }
	virtual bool	IterateMultivaluedNext ()						{ return false; }
	virtual bool	IterateKillListStart ( CSphString & )			{ return false; }
	virtual bool	IterateKillListNext ( SphDocID_t & )			{ return false; }

	void			Setup ( const CSphConfigSection & hSource, FILE * pPipe );

protected:
	enum ESphParseResult
	{
		PARSING_FAILED,
		GOT_DOCUMENT,
		DATA_OVER
	};

	BYTE **					ReportDocumentError();
	virtual void			SetupSchema ( const CSphConfigSection & hSource, bool bWordDict ) = 0;
	virtual ESphParseResult	SplitColumns ( CSphString & ) = 0;

	CSphVector<BYTE>			m_dBuf;
	CSphFixedVector<char>		m_dError;
	CSphFixedVector<int>		m_dColumnsLen;
	CSphFixedVector<RemapXSV_t>	m_dRemap;

	// output
	CSphFixedVector<BYTE *>		m_dFields;

	FILE *						m_pPipe;			///< incoming stream
	int							m_iDataStart;
	int							m_iDataLeft;
	int							m_iLine;
	int							m_iAutoCount;
};


class CSphSource_TSV : public CSphSource_BaseSV
{
public:
	explicit CSphSource_TSV ( const char * sName );
	virtual ~CSphSource_TSV();

	virtual ESphParseResult	SplitColumns ( CSphString & sError );					///< parse incoming chunk and emit some hits
	virtual void			SetupSchema ( const CSphConfigSection & hSource, bool bWordDict );
};


class CSphSource_CSV : public CSphSource_BaseSV
{
public:
	CSphSource_CSV ( const char * sName, const char * sDelimiter );
	virtual ~CSphSource_CSV();

	virtual ESphParseResult	SplitColumns ( CSphString & sError );					///< parse incoming chunk and emit some hits
	virtual void			SetupSchema ( const CSphConfigSection & hSource, bool bWordDict );

private:
	BYTE			m_iDelimiter;
};


CSphSource * sphCreateSourceTSVpipe ( const CSphConfigSection * pSource, FILE * pPipe, const char * sSourceName, bool , bool RLPARG(bProxy) )
{
	CSphSource_TSV * pTSV = NULL;
#if USE_RLP
	if ( bProxy )
		pTSV = new CSphSource_Proxy<CSphSource_TSV> ( sSourceName );
	else
#endif
		pTSV = new CSphSource_TSV ( sSourceName );

	pTSV->Setup ( *pSource, pPipe );
	return pTSV;
}


CSphSource * sphCreateSourceCSVpipe ( const CSphConfigSection * pSource, FILE * pPipe, const char * sSourceName, bool , bool RLPARG(bProxy) )
{
	CSphSource_CSV * pCSV = NULL;
	const char * sDelimiter = pSource->GetStr ( "csvpipe_delimiter", "" );

#if USE_RLP
	if ( bProxy )
		pCSV = new CSphSource_Proxy<CSphSource_CSV> ( sSourceName, sDelimiter );
	else
#endif
		pCSV = new CSphSource_CSV ( sSourceName, sDelimiter );

	pCSV->Setup ( *pSource, pPipe );
	return pCSV;
}


CSphSource_BaseSV::CSphSource_BaseSV ( const char * sName )
	: CSphSource_Document ( sName )
	, m_dError ( 1024 )
	, m_dColumnsLen ( 0 )
	, m_dRemap ( 0 )
	, m_dFields ( 0 )
	, m_iAutoCount ( 0 )
{
	m_iDataStart = 0;
	m_iDataLeft = 0;
}


CSphSource_BaseSV::~CSphSource_BaseSV ()
{
	Disconnect();
}

struct SortedRemapXSV_t : public RemapXSV_t
{
	int m_iTag;
};


void CSphSource_BaseSV::Setup ( const CSphConfigSection & hSource, FILE * pPipe )
{
	m_pPipe = pPipe;
	m_tSchema.Reset ();
	bool bWordDict = ( m_pDict && m_pDict->GetSettings().m_bWordDict );

	SetupSchema ( hSource, bWordDict );

	m_dFields.Reset ( m_tSchema.m_dFields.GetLength() );

	// build hash from schema names
	SmallStringHash_T<SortedRemapXSV_t> hSchema;
	SortedRemapXSV_t tElem;
	tElem.m_iTag = -1;
	tElem.m_iAttr = -1;
	ARRAY_FOREACH ( i, m_tSchema.m_dFields )
	{
		tElem.m_iField = i;
		hSchema.Add ( tElem, m_tSchema.m_dFields[i].m_sName );
	}
	tElem.m_iField = -1;
	for ( int i=0; i<m_tSchema.GetAttrsCount(); i++ )
	{
		RemapXSV_t * pRemap = hSchema ( m_tSchema.GetAttr ( i ).m_sName );
		if ( pRemap )
		{
			pRemap->m_iAttr = i;
		} else
		{
			tElem.m_iAttr = i;
			hSchema.Add ( tElem, m_tSchema.GetAttr ( i ).m_sName );
		}
	}

	// restore order for declared columns
	CSphString sTmp;
	hSource.IterateStart();
	while ( hSource.IterateNext() )
	{
		const CSphVariant * pVal = &hSource.IterateGet();
		while ( pVal )
		{
			const char * sOptionValue = pVal->cstr();
			// uint attribute might have bit count that should by cut off from name
			char * pColon = strchr ( const_cast<char*> ( sOptionValue ), ':' );
			if ( pColon )
			{
				sTmp.SetBinary ( sOptionValue, pColon-sOptionValue );
				sOptionValue = sTmp.cstr();
			}

			SortedRemapXSV_t * pColumn = hSchema ( sOptionValue );
			assert ( !pColumn || pColumn->m_iAttr>=0 || pColumn->m_iField>=0 );
			assert ( !pColumn || pColumn->m_iTag==-1 );
			if ( pColumn )
				pColumn->m_iTag = pVal->m_iTag;

			pVal = pVal->m_pNext;
		}
	}

	// fields + attributes + id - auto-generated
	m_dColumnsLen.Reset ( hSchema.GetLength() + 1 );
	m_dRemap.Reset ( hSchema.GetLength() + 1 );
	CSphFixedVector<SortedRemapXSV_t> dColumnsSorted ( hSchema.GetLength() );

	hSchema.IterateStart();
	for ( int i=0; hSchema.IterateNext(); i++ )
	{
		assert ( hSchema.IterateGet().m_iTag>=0 );
		dColumnsSorted[i] = hSchema.IterateGet();
	}

	sphSort ( dColumnsSorted.Begin(), dColumnsSorted.GetLength(), bind ( &SortedRemapXSV_t::m_iTag ) );

	// set remap incoming columns to fields \ attributes
	// doc_id dummy filler
	m_dRemap[0].m_iAttr = 0;
	m_dRemap[0].m_iField = 0;

	ARRAY_FOREACH ( i, dColumnsSorted )
	{
		assert ( !i || dColumnsSorted[i-1].m_iTag<dColumnsSorted[i].m_iTag ); // no duplicates allowed
		m_dRemap[i+1] = dColumnsSorted[i];
	}
}


bool CSphSource_BaseSV::Connect ( CSphString & sError )
{
	bool bWordDict = ( m_pDict && m_pDict->GetSettings().m_bWordDict );
	ARRAY_FOREACH ( i, m_tSchema.m_dFields )
	{
		CSphColumnInfo & tCol = m_tSchema.m_dFields[i];
		tCol.m_eWordpart = GetWordpart ( tCol.m_sName.cstr(), bWordDict );
	}

	int iAttrs = m_tSchema.GetAttrsCount();
	if ( !AddAutoAttrs ( sError ) )
		return false;

	m_iAutoCount = m_tSchema.GetAttrsCount() - iAttrs;

	AllocDocinfo();

	m_tHits.m_dData.Reserve ( m_iMaxHits );
	m_dBuf.Resize ( DEFAULT_READ_BUFFER );
	m_dMva.Reserve ( 512 );

	return true;
}


void CSphSource_BaseSV::Disconnect()
{
	if ( m_pPipe )
	{
		pclose ( m_pPipe );
		m_pPipe = NULL;
	}
	m_tHits.m_dData.Reset();
}


const char * CSphSource_BaseSV::DecorateMessage ( const char * sTemplate, ... ) const
{
	va_list ap;
	va_start ( ap, sTemplate );
	vsnprintf ( m_dError.Begin(), m_dError.GetLength(), sTemplate, ap );
	va_end ( ap );
	return m_dError.Begin();
}


bool CSphSource_BaseSV::IterateStart ( CSphString & sError )
{
	if ( !m_tSchema.m_dFields.GetLength() )
	{
		sError.SetSprintf ( "No fields in schema - will not index" );
		return false;
	}

	m_iLine = 0;
	m_iDataStart = 0;
	m_iDataLeft = 0;

	// initial buffer update
	int iRead = fread ( m_dBuf.Begin(), 1, m_dBuf.GetLength(), m_pPipe );
	if ( !iRead )
	{
		sError.SetSprintf ( "source '%s': read error '%s'", m_tSchema.m_sName.cstr(), strerror(errno) );
		return false;
	}
	m_iDataLeft = iRead;
	m_iPlainFieldsLength = m_tSchema.m_dFields.GetLength();

	return true;
}

BYTE ** CSphSource_BaseSV::ReportDocumentError ()
{
	m_tDocInfo.m_iDocID = 1; // 0 means legal eof
	m_iDataStart = 0;
	m_iDataLeft = 0;
	return NULL;
}


BYTE **	CSphSource_BaseSV::NextDocument ( CSphString & sError )
{
	ESphParseResult eRes = SplitColumns ( sError );
	if ( eRes==PARSING_FAILED )
		return ReportDocumentError();
	else if ( eRes==DATA_OVER )
		return NULL;

	assert ( eRes==GOT_DOCUMENT );

	// check doc_id
	if ( !m_dColumnsLen[0] )
	{
		sError.SetSprintf ( "source '%s': not doc_id found (line=%d, pos=%d, docid=" DOCID_FMT ")",
			m_tSchema.m_sName.cstr(), m_iLine, m_iDataStart, m_tDocInfo.m_iDocID );
		return ReportDocumentError();
	}

	// parse doc_id
	m_tDocInfo.m_iDocID = sphToDocid ( (const char *)m_dBuf.Begin() );

	// check doc_id
	if ( m_tDocInfo.m_iDocID==0 )
	{
		sError.SetSprintf ( "source '%s': invalid doc_id found (line=%d, pos=%d, docid=" DOCID_FMT ")",
			m_tSchema.m_sName.cstr(), m_iLine, m_iDataStart, m_tDocInfo.m_iDocID );
		return ReportDocumentError();
	}

	// parse column data
	int iOff = m_dColumnsLen[0] + 1;
	int iColumns = m_dRemap.GetLength();
	for ( int iCol=1; iCol<iColumns; iCol++ )
	{
		// if+if for field-string attribute case
		const RemapXSV_t & tRemap = m_dRemap[iCol];

		// field column
		if ( tRemap.m_iField!=-1 )
			m_dFields[tRemap.m_iField] = m_dBuf.Begin() + iOff;

		// attribute column
		if ( tRemap.m_iAttr!=-1 )
		{
			const CSphColumnInfo & tAttr = m_tSchema.GetAttr ( tRemap.m_iAttr );
			const char * sVal = (const char *)m_dBuf.Begin() + iOff;

			switch ( tAttr.m_eAttrType )
			{
			case SPH_ATTR_ORDINAL:
			case SPH_ATTR_STRING:
			case SPH_ATTR_JSON:
			case SPH_ATTR_WORDCOUNT:
				m_dStrAttrs[tRemap.m_iAttr] = sVal;
				m_tDocInfo.SetAttr ( tAttr.m_tLocator, 0 );
				break;

			case SPH_ATTR_FLOAT:
				m_tDocInfo.SetAttrFloat ( tAttr.m_tLocator, sphToFloat ( sVal ) );
				break;

			case SPH_ATTR_BIGINT:
				m_tDocInfo.SetAttr ( tAttr.m_tLocator, sphToInt64 ( sVal ) );
				break;

			case SPH_ATTR_UINT32SET:
			case SPH_ATTR_INT64SET:
				m_tDocInfo.SetAttr ( tAttr.m_tLocator, ParseFieldMVA ( m_dMva, sVal, ( tAttr.m_eAttrType==SPH_ATTR_INT64SET ) ) );
				break;

			case SPH_ATTR_TOKENCOUNT:
				m_tDocInfo.SetAttr ( tAttr.m_tLocator, 0 );
				break;

			default:
				m_tDocInfo.SetAttr ( tAttr.m_tLocator, sphToDword ( sVal ) );
				break;
			}
		}

		iOff += m_dColumnsLen[iCol] + 1; // length of value plus null-terminator
	}

	m_iLine++;
	return m_dFields.Begin();
}


CSphSource_TSV::CSphSource_TSV ( const char * sName )
: CSphSource_BaseSV ( sName )
{
}

CSphSource_TSV::~CSphSource_TSV()
{
}


CSphSource_BaseSV::ESphParseResult CSphSource_TSV::SplitColumns ( CSphString & sError )
{
	// move up tail to buffer head
	if ( m_iDataLeft && m_iDataStart )
	{
		memmove ( m_dBuf.Begin(), m_dBuf.Begin() + m_iDataStart, m_iDataLeft );
		m_iDataStart = 0;
	}

	int iColumns = m_dRemap.GetLength();
	int iCol = 0;
	int iColumnStart = 0;
	BYTE * pData = m_dBuf.Begin();
	const BYTE * pEnd = m_dBuf.Begin() + m_iDataLeft;

	for ( ;; )
	{
		if ( iCol>=iColumns )
		{
			sError.SetSprintf ( "source '%s': too many columns found (found=%d, declared=%d, line=%d, pos=%d, docid=" DOCID_FMT ")",
				m_tSchema.m_sName.cstr(), iCol, iColumns+m_iAutoCount, m_iLine, (int)( pData - m_dBuf.Begin() ), m_tDocInfo.m_iDocID );
			return CSphSource_BaseSV::PARSING_FAILED;
		}

		// move to next control symbol
		while ( pData<pEnd && *pData && *pData!='\t' && *pData!='\r' && *pData!='\n' )
			pData++;

		if ( pData<pEnd )
		{
			assert ( *pData=='\t' || !*pData || *pData=='\r' || *pData=='\n' );
			bool bNull = !*pData;
			bool bEOL = ( *pData=='\r' || *pData=='\n' );

			int iLen = pData - m_dBuf.Begin() - iColumnStart;
			assert ( iLen>=0 );
			m_dColumnsLen[iCol] = iLen;
			*pData++ = '\0';
			iCol++;

			if ( bNull )
			{
				// null terminated string found
				m_iDataStart = m_iDataLeft = 0;
				break;
			} else if ( bEOL )
			{
				// end of document found
				// skip all EOL characters
				while ( pData<pEnd && *pData && ( *pData=='\r' || *pData=='\n' ) )
					pData++;
				break;
			}

			// column separator found
			iColumnStart = pData - m_dBuf.Begin();
			continue;
		}

		int iOff = pData - m_dBuf.Begin();

		// full buffer got resized to collect whole document
		if ( iOff==m_dBuf.GetLength() )
			m_dBuf.Resize ( m_dBuf.GetLength()*2 );

		int iGot = fread ( m_dBuf.Begin() + iOff, 1, m_dBuf.GetLength() - iOff, m_pPipe );
		if ( !iGot )
		{
			if ( !iCol )
			{
				// normal file termination - no pending columns and documents
				m_iDataStart = m_iDataLeft = 0;
				m_tDocInfo.m_iDocID = 0;
				return CSphSource_BaseSV::DATA_OVER;
			}

			// error in case no data left in middle of data stream
			sError.SetSprintf ( "source '%s': read error '%s' (line=%d, pos=%d, docid=" DOCID_FMT ")",
				m_tSchema.m_sName.cstr(), strerror(errno), m_iLine, iOff, m_tDocInfo.m_iDocID );
			return CSphSource_BaseSV::PARSING_FAILED;
		}

		// fix-up pointers due of buffer resize
		pData = m_dBuf.Begin() + iOff;
		pEnd = m_dBuf.Begin() + iOff + iGot;
	}

	// all columns presence check
	if ( iCol!=iColumns )
	{
		sError.SetSprintf ( "source '%s': not all columns found (found=%d, total=%d, line=%d, pos=%d, docid=" DOCID_FMT ")",
			m_tSchema.m_sName.cstr(), iCol, iColumns, m_iLine, (int)( pData - m_dBuf.Begin() ), m_tDocInfo.m_iDocID );
		return CSphSource_BaseSV::PARSING_FAILED;
	}

	// tail data
	assert ( pData<=pEnd );
	m_iDataStart = pData - m_dBuf.Begin();
	m_iDataLeft = pEnd - pData;
	return CSphSource_BaseSV::GOT_DOCUMENT;
}


void CSphSource_TSV::SetupSchema ( const CSphConfigSection & hSource, bool bWordDict )
{
	ConfigureAttrs ( hSource("tsvpipe_attr_uint"),			SPH_ATTR_INTEGER,	m_tSchema );
	ConfigureAttrs ( hSource("tsvpipe_attr_timestamp"),		SPH_ATTR_TIMESTAMP,	m_tSchema );
	ConfigureAttrs ( hSource("tsvpipe_attr_bool"),			SPH_ATTR_BOOL,		m_tSchema );
	ConfigureAttrs ( hSource("tsvpipe_attr_float"),			SPH_ATTR_FLOAT,		m_tSchema );
	ConfigureAttrs ( hSource("tsvpipe_attr_bigint"),		SPH_ATTR_BIGINT,	m_tSchema );
	ConfigureAttrs ( hSource("tsvpipe_attr_multi"),			SPH_ATTR_UINT32SET,	m_tSchema );
	ConfigureAttrs ( hSource("tsvpipe_attr_multi_64"),		SPH_ATTR_INT64SET,	m_tSchema );
	ConfigureAttrs ( hSource("tsvpipe_attr_string"),		SPH_ATTR_STRING,	m_tSchema );
	ConfigureAttrs ( hSource("tsvpipe_attr_json"),			SPH_ATTR_JSON,		m_tSchema );
	ConfigureAttrs ( hSource("tsvpipe_field_string"),		SPH_ATTR_STRING,	m_tSchema );

	ConfigureFields ( hSource("tsvpipe_field"), bWordDict, m_tSchema );
	ConfigureFields ( hSource("tsvpipe_field_string"), bWordDict, m_tSchema );
}


CSphSource_CSV::CSphSource_CSV ( const char * sName, const char * sDelimiter )
	: CSphSource_BaseSV ( sName )
{
	m_iDelimiter = BYTE ( ',' );
	if ( sDelimiter && *sDelimiter )
		m_iDelimiter = *sDelimiter;
}

CSphSource_CSV::~CSphSource_CSV()
{
}


CSphSource_BaseSV::ESphParseResult CSphSource_CSV::SplitColumns ( CSphString & sError )
{
	// move up tail to buffer head
	if ( m_iDataLeft && m_iDataStart )
	{
		memmove ( m_dBuf.Begin(), m_dBuf.Begin() + m_iDataStart, m_iDataLeft );
		m_iDataStart = 0;
	}

	int iColumns = m_dRemap.GetLength();
	int iCol = 0;
	int iColumnStart = 0;
	int iQuoteCount = 0;
	int iQuoteStart = -1;
	int	iEscapeStart = -1;
	const BYTE * pDst = m_dBuf.Begin();
	BYTE * pSrc = m_dBuf.Begin();
	const BYTE * pEnd = m_dBuf.Begin() + m_iDataLeft;

	for ( ;; )
	{
		assert ( pSrc<=pDst );
		// move to next control symbol
		while ( pDst<pEnd && *pDst && *pDst!=m_iDelimiter && *pDst!='"' && *pDst!='\\' && *pDst!='\r' && *pDst!='\n' )
			*pSrc++ = *pDst++;

		if ( pDst<pEnd )
		{
			assert ( !*pDst || *pDst==m_iDelimiter || *pDst=='"' || *pDst=='\\' || *pDst=='\r' || *pDst=='\n' );
			bool bNull = !*pDst;
			bool bEOL = ( *pDst=='\r' || *pDst=='\n' );
			bool bDelimiter = ( *pDst==m_iDelimiter );
			bool bQuot = ( *pDst=='"' );
			bool bEscape = ( *pDst=='\\' );
			int iOff = pDst-m_dBuf.Begin();

			// [ " ... " ]
			// [ " ... "" ... " ]
			// [ " ... """ ]
			// [ " ... """" ... " ]
			if ( bQuot || ( iQuoteCount%2 )==1 ) // quotation
			{
				// order of operations with quotation counter and offset matter
				if ( bQuot )
					iQuoteCount++;

				// any symbol inside quotation proceed as regular
				// but not quotation itself
				// but quoted quotation proceed as regular symbol
				bool bOdd = ( ( iQuoteCount%2 )==1 );
				if ( bOdd && ( !bQuot || ( iQuoteStart!=-1 && iQuoteStart+1==iOff ) ) ) // regular symbol inside quotation or quoted quotation
				{
						*pSrc++ = *pDst;
				}
				pDst++;

				if ( bQuot )
					iQuoteStart = iOff;

				continue;
			}

			if ( bEscape ) // not quoted escape symbol
			{
				if ( iEscapeStart>=0 && iEscapeStart+1==iOff ) // next to escape symbol proceed as regular
				{
					*pSrc++ = *pDst;
				} else // escape just started
				{
					iEscapeStart = iOff;
					pDst++;
				}
				continue;
			}

			int iLen = pSrc - m_dBuf.Begin() - iColumnStart;
			assert ( iLen>=0 );
			m_dColumnsLen[iCol] = iLen;
			*pSrc++ = '\0';
			pDst++;
			iCol++;

			if ( bNull ) // null terminated string found
			{
				m_iDataStart = m_iDataLeft = 0;
				break;
			} else if ( bEOL ) // end of document found
			{
				// skip all EOL characters
				while ( pDst<pEnd && *pDst && ( *pDst=='\r' || *pDst=='\n' ) )
					pDst++;
				break;
			}

			assert ( bDelimiter );
			// column separator found
			iColumnStart = pSrc - m_dBuf.Begin();
			continue;
		}

		int iDstOff = pDst - m_dBuf.Begin();
		int iSrcOff = pSrc - m_dBuf.Begin();

		// full buffer got resized to collect whole document
		if ( iDstOff==m_dBuf.GetLength() )
			m_dBuf.Resize ( m_dBuf.GetLength()*2 );

		int iGot = fread ( m_dBuf.Begin() + iDstOff, 1, m_dBuf.GetLength() - iDstOff, m_pPipe );
		if ( !iGot )
		{
			if ( !iCol )
			{
				// normal file termination - no pending columns and documents
				m_iDataStart = m_iDataLeft = 0;
				m_tDocInfo.m_iDocID = 0;
				return CSphSource_BaseSV::DATA_OVER;
			}

			// error in case no data left in middle of data stream
			sError.SetSprintf ( "source '%s': read error '%s' (line=%d, pos=%d, docid=" DOCID_FMT ")",
				m_tSchema.m_sName.cstr(), strerror(errno), m_iLine, iDstOff, m_tDocInfo.m_iDocID );
			return CSphSource_BaseSV::PARSING_FAILED;
		}

		// fix-up pointers due of buffer resize
		pDst = m_dBuf.Begin() + iDstOff;
		pSrc = m_dBuf.Begin() + iSrcOff;
		pEnd = m_dBuf.Begin() + iDstOff + iGot;
	}

	// all columns presence check
	if ( iCol!=iColumns )
	{
		sError.SetSprintf ( "source '%s': not all columns found (found=%d, total=%d, line=%d, pos=%d, docid=" DOCID_FMT ")",
			m_tSchema.m_sName.cstr(), iCol, iColumns, m_iLine, (int)( pDst - m_dBuf.Begin() ), m_tDocInfo.m_iDocID );
		return CSphSource_BaseSV::PARSING_FAILED;
	}

	// tail data
	assert ( pDst<=pEnd );
	m_iDataStart = pDst - m_dBuf.Begin();
	m_iDataLeft = pEnd - pDst;
	return CSphSource_BaseSV::GOT_DOCUMENT;
}


void CSphSource_CSV::SetupSchema ( const CSphConfigSection & hSource, bool bWordDict )
{
	ConfigureAttrs ( hSource("csvpipe_attr_uint"),			SPH_ATTR_INTEGER,	m_tSchema );
	ConfigureAttrs ( hSource("csvpipe_attr_timestamp"),		SPH_ATTR_TIMESTAMP,	m_tSchema );
	ConfigureAttrs ( hSource("csvpipe_attr_bool"),			SPH_ATTR_BOOL,		m_tSchema );
	ConfigureAttrs ( hSource("csvpipe_attr_float"),			SPH_ATTR_FLOAT,		m_tSchema );
	ConfigureAttrs ( hSource("csvpipe_attr_bigint"),		SPH_ATTR_BIGINT,	m_tSchema );
	ConfigureAttrs ( hSource("csvpipe_attr_multi"),			SPH_ATTR_UINT32SET,	m_tSchema );
	ConfigureAttrs ( hSource("csvpipe_attr_multi_64"),		SPH_ATTR_INT64SET,	m_tSchema );
	ConfigureAttrs ( hSource("csvpipe_attr_string"),		SPH_ATTR_STRING,	m_tSchema );
	ConfigureAttrs ( hSource("csvpipe_attr_json"),			SPH_ATTR_JSON,		m_tSchema );
	ConfigureAttrs ( hSource("csvpipe_field_string"),		SPH_ATTR_STRING,	m_tSchema );

	ConfigureFields ( hSource("csvpipe_field"), bWordDict, m_tSchema );
	ConfigureFields ( hSource("csvpipe_field_string"), bWordDict, m_tSchema );
}


/////////////////////////////////////////////////////////////////////////////

void sphSetQuiet ( bool bQuiet )
{
	g_bSphQuiet = bQuiet;
}


void sphSetDebugCheck ()
{
	g_bDebugCheck = true;
}


void sphSetJsonOptions ( bool bStrict, bool bAutoconvNumbers, bool bKeynamesToLowercase )
{
	g_bJsonStrict = bStrict;
	g_bJsonAutoconvNumbers = bAutoconvNumbers;
	g_bJsonKeynamesToLowercase = bKeynamesToLowercase;
}


static inline float GetPercent ( int64_t a, int64_t b )
{
	if ( b==0 )
		return 100.0f;

	int64_t r = a*100000/b;
	return float(r)/1000;
}


const char * CSphIndexProgress::BuildMessage() const
{
	static char sBuf[256];
	switch ( m_ePhase )
	{
		case PHASE_COLLECT:
			snprintf ( sBuf, sizeof(sBuf), "collected "INT64_FMT" docs, %.1f MB", m_iDocuments,
				float(m_iBytes)/1000000.0f );
			break;

		case PHASE_SORT:
			snprintf ( sBuf, sizeof(sBuf), "sorted %.1f Mhits, %.1f%% done", float(m_iHits)/1000000,
				GetPercent ( m_iHits, m_iHitsTotal ) );
			break;

		case PHASE_COLLECT_MVA:
			snprintf ( sBuf, sizeof(sBuf), "collected "INT64_FMT" attr values", m_iAttrs );
			break;

		case PHASE_SORT_MVA:
			snprintf ( sBuf, sizeof(sBuf), "sorted %.1f Mvalues, %.1f%% done", float(m_iAttrs)/1000000,
				GetPercent ( m_iAttrs, m_iAttrsTotal ) );
			break;

		case PHASE_MERGE:
			snprintf ( sBuf, sizeof(sBuf), "merged %.1f Kwords", float(m_iWords)/1000 );
			break;

		case PHASE_PREREAD:
			snprintf ( sBuf, sizeof(sBuf), "read %.1f of %.1f MB, %.1f%% done",
				float(m_iBytes)/1000000.0f, float(m_iBytesTotal)/1000000.0f,
				GetPercent ( m_iBytes, m_iBytesTotal ) );
			break;

		case PHASE_PRECOMPUTE:
			snprintf ( sBuf, sizeof(sBuf), "indexing attributes, %d.%d%% done", m_iDone/10, m_iDone%10 );
			break;

		default:
			assert ( 0 && "internal error: unhandled progress phase" );
			snprintf ( sBuf, sizeof(sBuf), "(progress-phase-%d)", m_ePhase );
			break;
	}

	sBuf[sizeof(sBuf)-1] = '\0';
	return sBuf;
}


void CSphIndexProgress::Show ( bool bPhaseEnd ) const
{
	if ( m_fnProgress )
		m_fnProgress ( this, bPhaseEnd );
}


/////////////////////////////////////////////////////////////////////////////

int sphDictCmp ( const char * pStr1, int iLen1, const char * pStr2, int iLen2 )
{
	assert ( pStr1 && pStr2 );
	assert ( iLen1 && iLen2 );
	const int iCmpLen = Min ( iLen1, iLen2 );
	return strncmp ( pStr1, pStr2, iCmpLen );
}

int sphDictCmpStrictly ( const char * pStr1, int iLen1, const char * pStr2, int iLen2 )
{
	assert ( pStr1 && pStr2 );
	assert ( iLen1 && iLen2 );
	const int iCmpLen = Min ( iLen1, iLen2 );
	const int iCmpRes = strncmp ( pStr1, pStr2, iCmpLen );
	return iCmpRes==0 ? iLen1-iLen2 : iCmpRes;
}


CWordlist::CWordlist ()
	: m_dCheckpoints ( 0 )
	, m_dInfixBlocks ( 0 )
{
	m_iDictCheckpointsOffset = 0;
	m_iSize = 0;
	m_iMaxChunk = 0;
	m_bWordDict = false;
	m_pWords = NULL;
	m_pInfixBlocksWords = NULL;
}

CWordlist::~CWordlist ()
{
	Reset();
}

void CWordlist::Reset ()
{
	m_tFile.Close ();
	m_pBuf.Reset ();

	m_dCheckpoints.Reset ( 0 );
	SafeDeleteArray ( m_pWords );
	SafeDeleteArray ( m_pInfixBlocksWords );
}

bool CWordlist::ReadCP ( CSphAutofile & tFile, DWORD uVersion, bool bWordDict, CSphString & sError )
{
	assert ( ( uVersion>=21 && bWordDict ) || !bWordDict );
	assert ( m_iDictCheckpointsOffset>0 );
	assert ( m_iSize-m_iDictCheckpointsOffset<UINT_MAX );

	m_bHaveSkips = ( uVersion>=31 );

	////////////////////////////
	// preload word checkpoints
	////////////////////////////

	int iCheckpointOnlySize = (int)(m_iSize-m_iDictCheckpointsOffset);
	if ( m_iInfixCodepointBytes && m_iInfixBlocksOffset )
		iCheckpointOnlySize = (int)(m_iInfixBlocksOffset - strlen ( g_sTagInfixBlocks ) - m_iDictCheckpointsOffset);

	CSphReader tReader;
	tReader.SetFile ( tFile );
	tReader.SeekTo ( m_iDictCheckpointsOffset, iCheckpointOnlySize );

	m_bWordDict = bWordDict;

	if ( m_bWordDict )
	{
		int iArenaSize = iCheckpointOnlySize
			- (sizeof(DWORD)+sizeof(SphOffset_t))*m_dCheckpoints.GetLength()
			+ sizeof(BYTE)*m_dCheckpoints.GetLength();
		assert ( iArenaSize>=0 );
		m_pWords = new BYTE[iArenaSize];
		assert ( m_pWords );

		BYTE * pWord = m_pWords;
		ARRAY_FOREACH ( i, m_dCheckpoints )
		{
			m_dCheckpoints[i].m_sWord = (char *)pWord;

			const int iLen = tReader.GetDword();
			assert ( iLen>0 );
			assert ( iLen+1+(pWord-m_pWords)<=iArenaSize );
			tReader.GetBytes ( pWord, iLen );
			pWord[iLen] = '\0';
			pWord += iLen+1;

			m_dCheckpoints[i].m_iWordlistOffset = tReader.GetOffset();
		}
	} else if ( uVersion>=11 )
	{
		// read v.14 checkpoints
		ARRAY_FOREACH ( i, m_dCheckpoints )
		{
			m_dCheckpoints[i].m_iWordID = (SphWordID_t)tReader.GetOffset();
			m_dCheckpoints[i].m_iWordlistOffset = tReader.GetOffset();
		}
	} else
	{
		// convert v.10 checkpoints
		ARRAY_FOREACH ( i, m_dCheckpoints )
		{
#if USE_64BIT
			m_dCheckpoints[i].m_iWordID = tReader.GetOffset();
#else
			m_dCheckpoints[i].m_iWordID = tReader.GetDword();
#endif
			m_dCheckpoints[i].m_iWordlistOffset = tReader.GetDword();
		}
	}

	////////////////////////
	// preload infix blocks
	////////////////////////

	if ( m_iInfixCodepointBytes && m_iInfixBlocksOffset )
	{
		// reading to vector as old version doesn't store total infix words length
		CSphTightVector<BYTE> dInfixWords;
		dInfixWords.Reserve ( (int)m_iInfixBlocksWordsSize );

		tReader.SeekTo ( m_iInfixBlocksOffset, (int)(m_iSize-m_iInfixBlocksOffset) );
		m_dInfixBlocks.Resize ( tReader.UnzipInt() );
		ARRAY_FOREACH ( i, m_dInfixBlocks )
		{
			int iBytes = tReader.UnzipInt();

			int iOff = dInfixWords.GetLength();
			m_dInfixBlocks[i].m_iInfixOffset = iOff;
			dInfixWords.Resize ( iOff+iBytes+1 );

			tReader.GetBytes ( dInfixWords.Begin()+iOff, iBytes );
			dInfixWords[iOff+iBytes] = '\0';

			m_dInfixBlocks[i].m_iOffset = tReader.UnzipInt();
		}

		// fix-up offset to pointer
		m_pInfixBlocksWords = dInfixWords.LeakData();
		ARRAY_FOREACH ( i, m_dInfixBlocks )
			m_dInfixBlocks[i].m_sInfix = (const char *)m_pInfixBlocksWords + m_dInfixBlocks[i].m_iInfixOffset;
	}

	// set wordlist end
	m_iWordsEnd = m_iDictCheckpointsOffset;
	if ( m_iInfixCodepointBytes && m_iInfixBlocksOffset )
	{
		if ( m_dInfixBlocks.GetLength() )
			m_iWordsEnd = m_dInfixBlocks.Begin()->m_iOffset - strlen ( g_sTagInfixEntries );
		else
			m_iWordsEnd -= strlen ( g_sTagInfixEntries );
	}

	// TODO: count m_dInfixBlocks too while make on_disk_dict work with dict=keywords + infix
	SphOffset_t uMaxChunk = 0;
	if ( m_dCheckpoints.GetLength() )
	{
		uMaxChunk = m_iWordsEnd - m_dCheckpoints.Last().m_iWordlistOffset;
		SphOffset_t uPrev = m_dCheckpoints.Begin()->m_iWordlistOffset;
		for ( int i=1; i<m_dCheckpoints.GetLength(); i++ )
		{
			SphOffset_t uOff = m_dCheckpoints[i].m_iWordlistOffset;
			uMaxChunk = Max ( uMaxChunk, uOff-uPrev );
			uPrev = uOff;
		}
	}
	assert ( uMaxChunk<UINT_MAX );
	m_iMaxChunk = (int)uMaxChunk;


	////////
	// done
	////////

	if ( tReader.GetErrorFlag() )
		sError = tReader.GetErrorMessage();
	return !tReader.GetErrorFlag();
}

const CSphWordlistCheckpoint * CWordlist::FindCheckpoint ( const char * sWord, int iWordLen, SphWordID_t iWordID, bool bStarMode ) const
{
	return sphSearchCheckpoint ( sWord, iWordLen, iWordID, bStarMode, m_bWordDict, m_dCheckpoints.Begin(), &m_dCheckpoints.Last() );
}


KeywordsBlockReader_c::KeywordsBlockReader_c ( const BYTE * pBuf, bool bSkips )
{
	m_pBuf = pBuf;
	m_sWord[0] = '\0';
	m_iLen = 0;
	m_bHaveSkips = bSkips;
}


bool KeywordsBlockReader_c::UnpackWord()
{
	if ( !m_pBuf )
		return false;

	// unpack next word
	// must be in sync with DictEnd()!
	BYTE uPack = *m_pBuf++;
	if ( !uPack )
	{
		// ok, this block is over
		m_pBuf = NULL;
		m_iLen = 0;
		return false;
	}

	int iMatch, iDelta;
	if ( uPack & 0x80 )
	{
		iDelta = ( ( uPack>>4 ) & 7 ) + 1;
		iMatch = uPack & 15;
	} else
	{
		iDelta = uPack & 127;
		iMatch = *m_pBuf++;
	}

	assert ( iMatch+iDelta<(int)sizeof(m_sWord)-1 );
	assert ( iMatch<=(int)strlen ( (char *)m_sWord ) );

	memcpy ( m_sWord + iMatch, m_pBuf, iDelta );
	m_pBuf += iDelta;

	m_iLen = iMatch + iDelta;
	m_sWord[m_iLen] = '\0';

	m_iDoclistOffset = sphUnzipOffset ( m_pBuf );
	m_iDocs = sphUnzipInt ( m_pBuf );
	m_iHits = sphUnzipInt ( m_pBuf );
	m_uHint = ( m_iDocs>=DOCLIST_HINT_THRESH ) ? *m_pBuf++ : 0;
	m_iDoclistHint = DoclistHintUnpack ( m_iDocs, m_uHint );
	if ( m_bHaveSkips && ( m_iDocs > SPH_SKIPLIST_BLOCK ) )
		m_iSkiplistOffset = sphUnzipInt ( m_pBuf );
	else
		m_iSkiplistOffset = 0;

	assert ( m_iLen>0 );
	return true;
}


bool CWordlist::GetWord ( const BYTE * pBuf, SphWordID_t iWordID, CSphDictEntry & tWord ) const
{
	SphWordID_t iLastID = 0;
	SphOffset_t uLastOff = 0;

	for ( ;; )
	{
		// unpack next word ID
		const SphWordID_t iDeltaWord = sphUnzipWordid ( pBuf ); // FIXME! slow with 32bit wordids

		if ( iDeltaWord==0 ) // wordlist chunk is over
			return false;

		iLastID += iDeltaWord;

		// list is sorted, so if there was no match, there's no such word
		if ( iLastID>iWordID )
			return false;

		// unpack next offset
		const SphOffset_t iDeltaOffset = sphUnzipOffset ( pBuf );
		uLastOff += iDeltaOffset;

		// unpack doc/hit count
		const int iDocs = sphUnzipInt ( pBuf );
		const int iHits = sphUnzipInt ( pBuf );
		SphOffset_t iSkiplistPos = 0;
		if ( m_bHaveSkips && ( iDocs > SPH_SKIPLIST_BLOCK ) )
			iSkiplistPos = sphUnzipOffset ( pBuf );

		assert ( iDeltaOffset );
		assert ( iDocs );
		assert ( iHits );

		// it matches?!
		if ( iLastID==iWordID )
		{
			sphUnzipWordid ( pBuf ); // might be 0 at checkpoint
			const SphOffset_t iDoclistLen = sphUnzipOffset ( pBuf );

			tWord.m_iDoclistOffset = uLastOff;
			tWord.m_iDocs = iDocs;
			tWord.m_iHits = iHits;
			tWord.m_iDoclistHint = (int)iDoclistLen;
			tWord.m_iSkiplistOffset = iSkiplistPos;
			return true;
		}
	}
}

const BYTE * CWordlist::AcquireDict ( const CSphWordlistCheckpoint * pCheckpoint, int iFD, BYTE * pDictBuf ) const
{
	assert ( pCheckpoint );
	assert ( m_dCheckpoints.GetLength() );
	assert ( pCheckpoint>=m_dCheckpoints.Begin() && pCheckpoint<=&m_dCheckpoints.Last() );
	assert ( pCheckpoint->m_iWordlistOffset>0 && pCheckpoint->m_iWordlistOffset<=m_iSize );
	assert ( m_pBuf.IsEmpty() || pCheckpoint->m_iWordlistOffset<(int64_t)m_pBuf.GetLengthBytes() );

	// TODO: implement on_disk_dict = 1 for dict=keywords + infix
	const BYTE * pBuf = NULL;

	if ( !m_pBuf.IsEmpty() )
		pBuf = m_pBuf.GetWritePtr()+pCheckpoint->m_iWordlistOffset;
	else
	{
		assert ( pDictBuf );
		SphOffset_t iChunkLength = 0;

		// not the end?
		if ( pCheckpoint < &m_dCheckpoints.Last() )
			iChunkLength = pCheckpoint[1].m_iWordlistOffset - pCheckpoint->m_iWordlistOffset;
		else
			iChunkLength = m_iWordsEnd - pCheckpoint->m_iWordlistOffset;

		assert ( iChunkLength<=m_iMaxChunk );
		if ( (int)sphPread ( iFD, pDictBuf, (size_t)iChunkLength, pCheckpoint->m_iWordlistOffset )==iChunkLength )
			pBuf = pDictBuf;
	}

	return pBuf;
}


int sphGetExpansionMagic ( int iDocs, int iHits )
{
	if ( iHits<=256 ) // magic threshold; mb make this configurable?
		return 1;
	else
		return iDocs + 1;
}


static inline void AddExpansion ( CSphVector<CSphNamedInt> & dExpanded, const KeywordsBlockReader_c & tCtx )
{
	assert ( tCtx.GetWordLen() );

	CSphNamedInt & tRes = dExpanded.Add();
	tRes.m_sName = tCtx.GetWord();
	tRes.m_iValue = sphGetExpansionMagic ( tCtx.m_iDocs, tCtx.m_iHits );
}


void CWordlist::GetPrefixedWords ( const char * sPrefix, int iPrefixLen, const char * sWildcard,
	CSphVector<CSphNamedInt> & dExpanded, BYTE * pDictBuf, int iFD ) const
{
	assert ( sPrefix && *sPrefix && iPrefixLen>0 );
	assert ( sWildcard && *sWildcard );

	// empty index?
	if ( !m_dCheckpoints.GetLength() )
		return;

	const CSphWordlistCheckpoint * pCheckpoint = FindCheckpoint ( sPrefix, iPrefixLen, 0, true );
	const int iSkipMagic = ( BYTE(*sPrefix)<0x20 ); // whether to skip heading magic chars in the prefix, like NONSTEMMED maker
	while ( pCheckpoint )
	{
		// decode wordlist chunk
		KeywordsBlockReader_c tCtx ( AcquireDict ( pCheckpoint, iFD, pDictBuf ), m_bHaveSkips );
		while ( tCtx.UnpackWord() )
		{
			// block is sorted
			// so once keywords are greater than the prefix, no more matches
			int iCmp = sphDictCmp ( sPrefix, iPrefixLen, tCtx.GetWord(), tCtx.GetWordLen() );
			if ( iCmp<0 )
				break;

			// does it match the prefix *and* the entire wildcard?
			if ( iCmp==0 && sphWildcardMatch ( tCtx.GetWord() + iSkipMagic, sWildcard ) )
				AddExpansion ( dExpanded, tCtx );
		}

		pCheckpoint++;
		if ( pCheckpoint > &m_dCheckpoints.Last() )
			break;

		if ( sphDictCmp ( sPrefix, iPrefixLen, pCheckpoint->m_sWord, strlen ( pCheckpoint->m_sWord ) )<0 )
			break;
	}
}

bool operator < ( const InfixBlock_t & a, const char * b )
{
	return strcmp ( a.m_sInfix, b )<0;
}

bool operator == ( const InfixBlock_t & a, const char * b )
{
	return strcmp ( a.m_sInfix, b )==0;
}

bool operator < ( const char * a, const InfixBlock_t & b )
{
	return strcmp ( a, b.m_sInfix )<0;
}


bool sphLookupInfixCheckpoints ( const char * sInfix, int iBytes, const BYTE * pInfixes, const CSphVector<InfixBlock_t> & dInfixBlocks, int iInfixCodepointBytes, CSphVector<int> & dCheckpoints )
{
	assert ( pInfixes );
	dCheckpoints.Resize ( 0 );

	// lookup block
	int iBlock = FindSpan ( dInfixBlocks, sInfix );
	if ( iBlock<0 )
		return false;
	const BYTE * pBlock = pInfixes + dInfixBlocks[iBlock].m_iOffset;

	// decode block and check for exact infix match
	// block entry is { byte edit_code, byte[] key_append, zint data_len, zint data_deltas[] }
	// zero edit_code marks block end
	BYTE sKey[32];
	for ( ;; )
	{
		// unpack next key
		int iCode = *pBlock++;
		if ( !iCode )
			break;

		BYTE * pOut = sKey;
		if ( iInfixCodepointBytes==1 )
		{
			pOut = sKey + ( iCode>>4 );
			iCode &= 15;
			while ( iCode-- )
				*pOut++ = *pBlock++;
		} else
		{
			int iKeep = ( iCode>>4 );
			while ( iKeep-- )
				pOut += sphUtf8CharBytes ( *pOut ); ///< wtf? *pOut (=sKey) is NOT initialized?
			assert ( pOut-sKey<=(int)sizeof(sKey) );
			iCode &= 15;
			while ( iCode-- )
			{
				int i = sphUtf8CharBytes ( *pBlock );
				while ( i-- )
					*pOut++ = *pBlock++;
			}
			assert ( pOut-sKey<=(int)sizeof(sKey) );
		}
		assert ( pOut-sKey<(int)sizeof(sKey) );
#ifndef NDEBUG
		*pOut = '\0'; // handy for debugging, but not used for real matching
#endif

		if ( pOut==sKey+iBytes && memcmp ( sKey, sInfix, iBytes )==0 )
		{
			// found you! decompress the data
			int iLast = 0;
			int iPackedLen = sphUnzipInt ( pBlock );
			const BYTE * pMax = pBlock + iPackedLen;
			while ( pBlock<pMax )
			{
				iLast += sphUnzipInt ( pBlock );
				dCheckpoints.Add ( iLast );
			}
			return true;
		}

		int iSkip = sphUnzipInt ( pBlock );
		pBlock += iSkip;
	}
	return false;
}


// calculate length, upto iInfixCodepointBytes chars from infix start
int sphGetInfixLength ( const char * sInfix, int iBytes, int iInfixCodepointBytes )
{
	int iBytes1 = Min ( 6, iBytes );
	if ( iInfixCodepointBytes!=1 )
	{
		int iCharsLeft = 6;
		const char * s = sInfix;
		const char * sMax = sInfix + iBytes;
		while ( iCharsLeft-- && s<sMax )
			s += sphUtf8CharBytes(*s);
		iBytes1 = (int)( s - sInfix );
	}

	return iBytes1;
}


void CWordlist::GetInfixedWords ( const char * sInfix, int iBytes, const char * sWildcard, CSphVector<CSphNamedInt> & dExpanded ) const
{
	// dict must be of keywords type, and fully cached
	// mmap()ed in the worst case, should we ever banish it to disk again
	if ( m_pBuf.IsEmpty() || !m_dCheckpoints.GetLength() )
		return;

	// extract key1, upto 6 chars from infix start
	int iBytes1 = sphGetInfixLength ( sInfix, iBytes, m_iInfixCodepointBytes );

	// lookup key1
	// OPTIMIZE? maybe lookup key2 and reduce checkpoint set size, if possible?
	CSphVector<int> dPoints;
	if ( !sphLookupInfixCheckpoints ( sInfix, iBytes1, m_pBuf.GetWritePtr(), m_dInfixBlocks, m_iInfixCodepointBytes, dPoints ) )
		return;

	// walk those checkpoints, check all their words
	ARRAY_FOREACH ( i, dPoints )
	{
		// OPTIMIZE? add a quicker path than a generic wildcard for "*infix*" case?
		KeywordsBlockReader_c tCtx ( m_pBuf.GetWritePtr() + m_dCheckpoints[dPoints[i]-1].m_iWordlistOffset, m_bHaveSkips );
		while ( tCtx.UnpackWord() )
			if ( sphWildcardMatch ( tCtx.GetWord(), sWildcard ) )
				AddExpansion ( dExpanded, tCtx );
	}
}


// all indexes should produce same terms for same query
void SphWordStatChecker_t::Set ( const SmallStringHash_T<CSphQueryResultMeta::WordStat_t> & hStat )
{
	m_dSrcWords.Reserve ( hStat.GetLength() );
	hStat.IterateStart();
	while ( hStat.IterateNext() )
	{
		if ( hStat.IterateGet().m_bExpanded )
			continue;

		m_dSrcWords.Add ( sphFNV64 ( (const BYTE*)hStat.IterateGetKey().cstr() ) );
	}
	m_dSrcWords.Sort();
}


void SphWordStatChecker_t::DumpDiffer ( const SmallStringHash_T<CSphQueryResultMeta::WordStat_t> & hStat, const char * sIndex, CSphString & sWarning )
{
	if ( !m_dSrcWords.GetLength() )
		return;

	bool bGotHead = false;

	hStat.IterateStart();
	while ( hStat.IterateNext() )
	{
		if ( hStat.IterateGet().m_bExpanded )
			continue;

		uint64_t uHash = sphFNV64 ( (const BYTE *)hStat.IterateGetKey().cstr() );
		if ( !m_dSrcWords.BinarySearch ( uHash ) )
		{
			if ( !bGotHead )
			{
				sWarning.SetSprintf ( "index '%s': query word(s) mismatch: %s", sIndex, hStat.IterateGetKey().cstr() );
				bGotHead = true;
			} else
			{
				sWarning.SetSprintf ( "%s, %s", sWarning.cstr(), hStat.IterateGetKey().cstr() );
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// CSphQueryResultMeta
//////////////////////////////////////////////////////////////////////////

CSphQueryResultMeta::CSphQueryResultMeta ()
	: m_iQueryTime ( 0 )
	, m_iRealQueryTime ( 0 )
	, m_iCpuTime ( 0 )
	, m_iMultiplier ( 1 )
	, m_iMatches ( 0 )
	, m_iTotalMatches ( 0 )
	, m_iAgentCpuTime ( 0 )
	, m_iPredictedTime ( 0 )
	, m_iAgentPredictedTime ( 0 )
	, m_iAgentFetchedDocs ( 0 )
	, m_iAgentFetchedHits ( 0 )
	, m_iAgentFetchedSkips ( 0 )
	, m_bHasPrediction ( false )
{
}


void CSphQueryResultMeta::AddStat ( const CSphString & sWord, int64_t iDocs, int64_t iHits, bool bExpanded )
{
	CSphString sFixed;
	const CSphString * pFixed = &sWord;
	if ( sWord.cstr()[0]==MAGIC_WORD_HEAD )
	{
		sFixed = sWord;
		*(char *)( sFixed.cstr() ) = '*';
		pFixed = &sFixed;
	} else if ( sWord.cstr()[0]==MAGIC_WORD_HEAD_NONSTEMMED )
	{
		if ( !bExpanded )
		{
			sFixed = sWord;
			*(char *)( sFixed.cstr() ) = '=';
			pFixed = &sFixed;
		} else
		{
			sFixed = sWord.SubString ( 1, sWord.Length()-1 );
			pFixed = &sFixed;
		}
	} else
	{
		const char * p = strchr ( sWord.cstr(), MAGIC_WORD_BIGRAM );
		if ( p )
		{
			sFixed.SetSprintf ( "\"%s\"", sWord.cstr() );
			*( (char*)sFixed.cstr() + ( p - sWord.cstr() ) + 1 ) = ' ';
			pFixed = &sFixed;
		}
	}

	WordStat_t * pStats = m_hWordStats ( *pFixed );
	if ( !pStats )
	{
		CSphQueryResultMeta::WordStat_t tStats;
		tStats.m_iDocs = iDocs;
		tStats.m_iHits = iHits;
		tStats.m_bExpanded = bExpanded;
		m_hWordStats.Add ( tStats, *pFixed );
	} else
	{
		pStats->m_iDocs += iDocs;
		pStats->m_iHits += iHits;
		pStats->m_bExpanded |= bExpanded;
	}
}


//////////////////////////////////////////////////////////////////////////
// CONVERSION TOOLS HELPERS
//////////////////////////////////////////////////////////////////////////

static void CopyBytes ( CSphWriter & wrTo, CSphReader & rdFrom, int iBytes )
{
	const int BUFSIZE = 65536;
	BYTE * pBuf = new BYTE [ BUFSIZE ];

	int iCopied = 0;
	while ( iCopied < iBytes )
	{
		int iToCopy = Min ( iBytes - iCopied, BUFSIZE );
		rdFrom.GetBytes ( pBuf, iToCopy );
		wrTo.PutBytes ( pBuf, iToCopy );
		iCopied += iToCopy;
	}

	SafeDeleteArray ( pBuf );
}


/// post-conversion chores
/// rename the files, show elapsed time
static void FinalizeUpgrade ( const char ** sRenames, const char * sBanner, const char * sPath, int64_t tmStart )
{
	while ( *sRenames )
	{
		CSphString sFrom, sTo;
		sFrom.SetSprintf ( "%s%s", sPath, sRenames[0] );
		sTo.SetSprintf ( "%s%s", sPath, sRenames[1] );
		sRenames += 2;

		if ( ::rename ( sFrom.cstr(), sTo.cstr() ) )
			sphDie ( "%s: rename %s to %s failed: %s\n", sBanner,
			sFrom.cstr(), sTo.cstr(), strerror(errno) );
	}

	// all done! yay
	int64_t tmWall = sphMicroTimer() - tmStart;
	fprintf ( stdout, "%s: elapsed %d.%d sec\n", sBanner,
		(int)(tmWall/1000000), (int)((tmWall/100000)%10) );
	fprintf ( stdout, "%s: done!\n", sBanner );
}

//////////////////////////////////////////////////////////////////////////
// V.26 TO V.27 CONVERSION TOOL, INFIX BUILDER
//////////////////////////////////////////////////////////////////////////

void sphDictBuildInfixes ( const char * sPath )
{
	CSphString sFilename, sError;
	int64_t tmStart = sphMicroTimer();

	if_const ( INDEX_FORMAT_VERSION!=27 )
		sphDie ( "infix upgrade: only works in v.27 builds for now; get an older indextool or contact support" );

	//////////////////////////////////////////////////
	// load (interesting parts from) the index header
	//////////////////////////////////////////////////

	CSphAutoreader rdHeader;
	sFilename.SetSprintf ( "%s.sph", sPath );
	if ( !rdHeader.Open ( sFilename.cstr(), sError ) )
		sphDie ( "infix upgrade: %s", sError.cstr() );

	// version
	DWORD uHeader = rdHeader.GetDword ();
	DWORD uVersion = rdHeader.GetDword();
	bool bUse64 = ( rdHeader.GetDword()!=0 );
	ESphDocinfo eDocinfo = (ESphDocinfo) rdHeader.GetDword();

	if ( uHeader!=INDEX_MAGIC_HEADER )
		sphDie ( "infix upgrade: invalid header file" );
	if ( uVersion<21 || uVersion>26 )
		sphDie ( "infix upgrade: got v.%d header, v.21 to v.26 required", uVersion );
	if ( eDocinfo==SPH_DOCINFO_INLINE )
		sphDie ( "infix upgrade: docinfo=inline is not supported" );

	CSphSchema tSchema;
	DictHeader_t tDictHeader;
	CSphSourceStats tStats;
	CSphIndexSettings tIndexSettings;
	CSphTokenizerSettings tTokenizerSettings;
	CSphDictSettings tDictSettings;
	CSphEmbeddedFiles tEmbeddedFiles;

	ReadSchema ( rdHeader, tSchema, uVersion, eDocinfo==SPH_DOCINFO_INLINE );
	SphOffset_t iMinDocid = rdHeader.GetOffset();
	tDictHeader.m_iDictCheckpointsOffset = rdHeader.GetOffset ();
	tDictHeader.m_iDictCheckpoints = rdHeader.GetDword ();
	tDictHeader.m_iInfixCodepointBytes = 0;
	tDictHeader.m_iInfixBlocksOffset = 0;
	tDictHeader.m_iInfixBlocksWordsSize = 0;
	tStats.m_iTotalDocuments = rdHeader.GetDword ();
	tStats.m_iTotalBytes = rdHeader.GetOffset ();
	LoadIndexSettings ( tIndexSettings, rdHeader, uVersion );
	LoadTokenizerSettings ( rdHeader, tTokenizerSettings, tEmbeddedFiles, uVersion, sError );
	LoadDictionarySettings ( rdHeader, tDictSettings, tEmbeddedFiles, uVersion, sError );
	int iKillListSize = rdHeader.GetDword();
	DWORD uMinMaxIndex = rdHeader.GetDword();

	if ( rdHeader.GetErrorFlag() )
		sphDie ( "infix upgrade: failed to parse header" );
	rdHeader.Close();

	////////////////////
	// generate infixes
	////////////////////

	if ( !tDictSettings.m_bWordDict )
		sphDie ( "infix upgrade: dict=keywords required" );

	tIndexSettings.m_iMinPrefixLen = 0;
	tIndexSettings.m_iMinInfixLen = 2;

	ISphTokenizer * pTokenizer = ISphTokenizer::Create ( tTokenizerSettings, &tEmbeddedFiles, sError );
	if ( !pTokenizer )
		sphDie ( "infix upgrade: %s", sError.cstr() );

	tDictHeader.m_iInfixCodepointBytes = pTokenizer->GetMaxCodepointLength();
	ISphInfixBuilder * pInfixer = sphCreateInfixBuilder ( tDictHeader.m_iInfixCodepointBytes, &sError );
	if ( !pInfixer )
		sphDie ( "infix upgrade: %s", sError.cstr() );

	// scan all dict entries, generate infixes
	// (in a separate block, so that tDictReader gets destroyed, and file closed)
	{
		CSphDictReader tDictReader;
		if ( !tDictReader.Setup ( sFilename.SetSprintf ( "%s.spi", sPath ),
			tDictHeader.m_iDictCheckpointsOffset, tIndexSettings.m_eHitless, sError, true, &g_tThrottle, uVersion>=31 ) )
				sphDie ( "infix upgrade: %s", sError.cstr() );
		while ( tDictReader.Read() )
		{
			const BYTE * sWord = tDictReader.GetWord();
			int iLen = strlen ( (const char *)sWord );
			pInfixer->AddWord ( sWord, iLen, tDictReader.GetCheckpoint() );
		}
	}

	/////////////////////////////
	// write new dictionary file
	/////////////////////////////

	// ready to party
	// open all the cans!
	CSphAutofile fdDict;
	fdDict.Open ( sFilename, SPH_O_READ, sError );

	CSphReader rdDict;
	rdDict.SetFile ( fdDict );
	rdDict.SeekTo ( 0, READ_NO_SIZE_HINT );

	CSphWriter wrDict;
	sFilename.SetSprintf ( "%s.spi.upgrade", sPath );
	if ( !wrDict.OpenFile ( sFilename, sError ) )
		sphDie ( "infix upgrade: failed to open %s", sFilename.cstr() );

	// copy the keyword entries until checkpoints
	CopyBytes ( wrDict, rdDict, (int)tDictHeader.m_iDictCheckpointsOffset );

	// write newly generated infix hash entries
	pInfixer->SaveEntries ( wrDict );

	// copy checkpoints
	int iCheckpointsSize = (int)( fdDict.GetSize() - tDictHeader.m_iDictCheckpointsOffset );
	tDictHeader.m_iDictCheckpointsOffset = wrDict.GetPos();
	CopyBytes ( wrDict, rdDict, iCheckpointsSize );

	// write newly generated infix hash blocks
	tDictHeader.m_iInfixBlocksOffset = pInfixer->SaveEntryBlocks ( wrDict );
	tDictHeader.m_iInfixBlocksWordsSize = pInfixer->GetBlocksWordsSize();

	// flush header
	// mostly for debugging convenience
	// primary storage is in the index wide header
	wrDict.PutBytes ( "dict-header", 11 );
	wrDict.ZipInt ( tDictHeader.m_iDictCheckpoints );
	wrDict.ZipOffset ( tDictHeader.m_iDictCheckpointsOffset );
	wrDict.ZipInt ( tDictHeader.m_iInfixCodepointBytes );
	wrDict.ZipInt ( tDictHeader.m_iInfixBlocksOffset );

	wrDict.CloseFile ();
	if ( wrDict.IsError() )
		sphDie ( "infix upgrade: dictionary write error (out of space?)" );

	if ( rdDict.GetErrorFlag() )
		sphDie ( "infix upgrade: dictionary read error" );
	fdDict.Close();

	////////////////////
	// write new header
	////////////////////

	assert ( tDictSettings.m_bWordDict );
	CSphDict * pDict = sphCreateDictionaryKeywords ( tDictSettings, &tEmbeddedFiles, pTokenizer, "$indexname", sError );
	if ( !pDict )
		sphDie ( "infix upgrade: %s", sError.cstr() );

	CSphWriter wrHeader;
	sFilename.SetSprintf ( "%s.sph.upgrade", sPath );
	if ( !wrHeader.OpenFile ( sFilename, sError ) )
		sphDie ( "infix upgrade: %s", sError.cstr() );

	wrHeader.PutDword ( INDEX_MAGIC_HEADER );
	wrHeader.PutDword ( INDEX_FORMAT_VERSION );
	wrHeader.PutDword ( bUse64 );
	wrHeader.PutDword ( eDocinfo );
	WriteSchema ( wrHeader, tSchema );
	wrHeader.PutOffset ( iMinDocid );
	wrHeader.PutOffset ( tDictHeader.m_iDictCheckpointsOffset );
	wrHeader.PutDword ( tDictHeader.m_iDictCheckpoints );
	wrHeader.PutByte ( tDictHeader.m_iInfixCodepointBytes );
	wrHeader.PutDword ( tDictHeader.m_iInfixBlocksOffset );
	wrHeader.PutDword ( tDictHeader.m_iInfixBlocksWordsSize );
	wrHeader.PutDword ( (DWORD)tStats.m_iTotalDocuments ); // FIXME? we don't expect over 4G docs per just 1 local index
	wrHeader.PutOffset ( tStats.m_iTotalBytes );
	SaveIndexSettings ( wrHeader, tIndexSettings );
	SaveTokenizerSettings ( wrHeader, pTokenizer, tIndexSettings.m_iEmbeddedLimit );
	SaveDictionarySettings ( wrHeader, pDict, false, tIndexSettings.m_iEmbeddedLimit );
	wrHeader.PutDword ( iKillListSize );
	wrHeader.PutDword ( uMinMaxIndex );
	wrHeader.PutDword ( 0 ); // no field filter

	wrHeader.CloseFile ();
	if ( wrHeader.IsError() )
		sphDie ( "infix upgrade: header write error (out of space?)" );

	// all done!
	const char * sRenames[] = {
		".sph", ".sph.bak",
		".spi", ".spi.bak",
		".sph.upgrade", ".sph",
		".spi.upgrade", ".spi",
		NULL };
	FinalizeUpgrade ( sRenames, "infix upgrade", sPath, tmStart );
}

//////////////////////////////////////////////////////////////////////////
// V.12 TO V.31 CONVERSION TOOL, SKIPLIST BUILDER
//////////////////////////////////////////////////////////////////////////

struct EntrySkips_t
{
	DWORD			m_uEntry;		///< sequential index in dict
	SphOffset_t		m_iDoclist;		///< doclist offset from dict
	int				m_iSkiplist;	///< generated skiplist offset
};

void sphDictBuildSkiplists ( const char * sPath )
{
	CSphString sFilename, sError;
	int64_t tmStart = sphMicroTimer();

	if_const ( INDEX_FORMAT_VERSION<31 || INDEX_FORMAT_VERSION>35 )
		sphDie ( "skiplists upgrade: ony works in v.31 to v.35 builds for now; get an older indextool or contact support" );

	// load (interesting parts from) the index header
	CSphAutoreader rdHeader;
	sFilename.SetSprintf ( "%s.sph", sPath );
	if ( !rdHeader.Open ( sFilename.cstr(), sError ) )
		sphDie ( "skiplists upgrade: %s", sError.cstr() );

	// version
	DWORD uHeader = rdHeader.GetDword ();
	DWORD uVersion = rdHeader.GetDword();
	bool bUse64 = ( rdHeader.GetDword()!=0 );
	bool bConvertCheckpoints = ( uVersion<=21 );
	ESphDocinfo eDocinfo = (ESphDocinfo) rdHeader.GetDword();
	const DWORD uLowestVersion = 12;

	if ( bUse64!=USE_64BIT )
		sphDie ( "skiplists upgrade: USE_64BIT differs, index %s, binary %s",
			bUse64 ? "enabled" : "disabled", USE_64BIT ? "enabled" : "disabled" );
	if ( uHeader!=INDEX_MAGIC_HEADER )
		sphDie ( "skiplists upgrade: invalid header file" );
	if ( uVersion<uLowestVersion )
		sphDie ( "skiplists upgrade: got v.%d header, v.%d to v.30 required", uVersion, uLowestVersion );
	if ( eDocinfo==SPH_DOCINFO_INLINE )
		sphDie ( "skiplists upgrade: docinfo=inline is not supported yet" );

	CSphSchema tSchema;
	DictHeader_t tDictHeader;
	CSphSourceStats tStats;
	CSphIndexSettings tIndexSettings;
	CSphTokenizerSettings tTokenizerSettings;
	CSphDictSettings tDictSettings;
	CSphEmbeddedFiles tEmbeddedFiles;

	ReadSchema ( rdHeader, tSchema, uVersion, eDocinfo==SPH_DOCINFO_INLINE );
	SphOffset_t iMinDocid = rdHeader.GetOffset();
	tDictHeader.m_iDictCheckpointsOffset = rdHeader.GetOffset ();
	tDictHeader.m_iDictCheckpoints = rdHeader.GetDword ();
	tDictHeader.m_iInfixCodepointBytes = 0;
	tDictHeader.m_iInfixBlocksOffset = 0;
	if ( uVersion>=27 )
	{
		tDictHeader.m_iInfixCodepointBytes = rdHeader.GetByte();
		tDictHeader.m_iInfixBlocksOffset = rdHeader.GetDword();
	}
	if ( uVersion>=34 )
		tDictHeader.m_iInfixBlocksWordsSize = rdHeader.GetDword();

	tStats.m_iTotalDocuments = rdHeader.GetDword ();
	tStats.m_iTotalBytes = rdHeader.GetOffset ();
	LoadIndexSettings ( tIndexSettings, rdHeader, uVersion );
	LoadTokenizerSettings ( rdHeader, tTokenizerSettings, tEmbeddedFiles, uVersion, sError );
	LoadDictionarySettings ( rdHeader, tDictSettings, tEmbeddedFiles, uVersion, sError );
	int iKillListSize = rdHeader.GetDword();

	SphOffset_t uMinMaxIndex = 0;
	if ( uVersion>=33 )
		uMinMaxIndex = rdHeader.GetOffset ();
	else if ( uVersion>=20 )
		uMinMaxIndex = rdHeader.GetDword ();

	ISphFieldFilter * pFieldFilter = NULL;
	if ( uVersion>=28 )
	{
		CSphFieldFilterSettings tFieldFilterSettings;
		LoadFieldFilterSettings ( rdHeader, tFieldFilterSettings );
		if ( tFieldFilterSettings.m_dRegexps.GetLength() )
			pFieldFilter = sphCreateFieldFilter ( tFieldFilterSettings, sError );
	}

	CSphFixedVector<uint64_t> dFieldLens ( tSchema.m_dFields.GetLength() );
	if ( uVersion>=35 && tIndexSettings.m_bIndexFieldLens )
		ARRAY_FOREACH ( i, tSchema.m_dFields )
			dFieldLens[i] = rdHeader.GetOffset(); // FIXME? ideally 64bit even when off is 32bit..

	if ( rdHeader.GetErrorFlag() )
		sphDie ( "skiplists upgrade: failed to parse header" );
	rdHeader.Close();

	//////////////////////
	// generate skiplists
	//////////////////////

	// keywords on disk might be in a different order than dictionary
	// and random accesses on a plain disk would be extremely slow
	// so we load the dictionary, sort by doclist offset
	// then we walk doclists, generate skiplists, sort back by entry number
	// then walk the disk dictionary again, lookup skiplist offset, and patch

	// load the dictionary
	CSphVector<EntrySkips_t> dSkips;
	const bool bWordDict = tDictSettings.m_bWordDict;

	CSphAutoreader rdDict;
	if ( !rdDict.Open ( sFilename.SetSprintf ( "%s.spi", sPath ), sError ) )
		sphDie ( "skiplists upgrade: %s", sError.cstr() );

	// compute actual keyword data length
	SphOffset_t iWordsEnd = tDictHeader.m_iDictCheckpointsOffset;
	if ( bWordDict && tDictHeader.m_iInfixCodepointBytes )
	{
		rdDict.SeekTo ( tDictHeader.m_iInfixBlocksOffset, 32 ); // need just 1 entry, 32 bytes should be ok
		rdDict.UnzipInt(); // skip block count
		int iInfixLen = rdDict.GetByte();
		rdDict.SkipBytes ( iInfixLen );
		iWordsEnd = rdDict.UnzipInt() - strlen ( g_sTagInfixEntries );
		rdDict.SeekTo ( 0, READ_NO_SIZE_HINT );
	}

	CSphDictReader * pReader = new CSphDictReader();
	pReader->Setup ( &rdDict, iWordsEnd, tIndexSettings.m_eHitless, bWordDict, &g_tThrottle, uVersion>=31 );

	DWORD uEntry = 0;
	while ( pReader->Read() )
	{
		if ( pReader->m_iDocs > SPH_SKIPLIST_BLOCK )
		{
			EntrySkips_t & t = dSkips.Add();
			t.m_uEntry = uEntry;
			t.m_iDoclist = pReader->m_iDoclistOffset;
			t.m_iSkiplist = -1;
		}
		if ( ++uEntry==0 )
			sphDie ( "skiplists upgrade: dictionaries over 4B entries are not supported yet!" );
	}

	// sort by doclist offset
	dSkips.Sort ( sphMemberLess ( &EntrySkips_t::m_iDoclist ) );

	// walk doclists, create skiplists
	CSphAutoreader rdDocs;
	if ( !rdDocs.Open ( sFilename.SetSprintf ( "%s.spd", sPath ), sError ) )
		sphDie ( "skiplists upgrade: %s", sError.cstr() );

	CSphWriter wrSkips;
	if ( !wrSkips.OpenFile ( sFilename.SetSprintf ( "%s.spe.tmp", sPath ), sError ) )
		sphDie ( "skiplists upgrade: failed to create %s", sFilename.cstr() );
	wrSkips.PutByte ( 1 );

	int iDone = -1;
	CSphVector<SkiplistEntry_t> dSkiplist;
	ARRAY_FOREACH ( i, dSkips )
	{
		// seek to that keyword
		// OPTIMIZE? use length hint from dict too?
		rdDocs.SeekTo ( dSkips[i].m_iDoclist, READ_NO_SIZE_HINT );

		// decode interesting bits of doclist
		SphDocID_t uDocid = SphDocID_t ( iMinDocid );
		SphOffset_t uHitPosition = 0;
		DWORD uDocs = 0;

		for ( ;; )
		{
			// save current entry position
			SphOffset_t uPos = rdDocs.GetPos();

			// decode next entry
			SphDocID_t uDelta = rdDocs.UnzipDocid();
			if ( !uDelta )
				break;

			// build skiplist, aka save decoder state as needed
			if ( ( uDocs & ( SPH_SKIPLIST_BLOCK-1 ) )==0 )
			{
				SkiplistEntry_t & t = dSkiplist.Add();
				t.m_iBaseDocid = uDocid;
				t.m_iOffset = uPos;
				t.m_iBaseHitlistPos = uHitPosition;
			}
			uDocs++;

			// do decode
			uDocid += uDelta; // track delta-encoded docid
			if ( tIndexSettings.m_eHitFormat==SPH_HIT_FORMAT_INLINE )
			{
				DWORD uHits = rdDocs.UnzipInt();
				rdDocs.UnzipInt(); // skip hit field mask/data
				if ( uHits==1 )
				{
					rdDocs.UnzipInt(); // skip inlined field id
				} else
				{
					uHitPosition += rdDocs.UnzipOffset(); // track delta-encoded hitlist offset
				}
			} else
			{
				uHitPosition += rdDocs.UnzipOffset(); // track delta-encoded hitlist offset
				rdDocs.UnzipInt(); // skip hit field mask/data
				rdDocs.UnzipInt(); // skip hit count
			}
		}

		// alright, we built it, so save it
		assert ( uDocs>SPH_SKIPLIST_BLOCK );
		assert ( dSkiplist.GetLength() );

		dSkips[i].m_iSkiplist = (int)wrSkips.GetPos();
		SkiplistEntry_t tLast = dSkiplist[0];
		for ( int j=1; j<dSkiplist.GetLength(); j++ )
		{
			const SkiplistEntry_t & t = dSkiplist[j];
			assert ( t.m_iBaseDocid - tLast.m_iBaseDocid>=SPH_SKIPLIST_BLOCK );
			assert ( t.m_iOffset - tLast.m_iOffset>=4*SPH_SKIPLIST_BLOCK );
			wrSkips.ZipOffset ( t.m_iBaseDocid - tLast.m_iBaseDocid - SPH_SKIPLIST_BLOCK );
			wrSkips.ZipOffset ( t.m_iOffset - tLast.m_iOffset - 4*SPH_SKIPLIST_BLOCK );
			wrSkips.ZipOffset ( t.m_iBaseHitlistPos - tLast.m_iBaseHitlistPos );
			tLast = t;
		}
		dSkiplist.Resize ( 0 );

		// progress bar
		int iDone2 = (1+i)*100 / dSkips.GetLength();
		if ( iDone2!=iDone )
		{
			iDone = iDone2;
			fprintf ( stdout, "skiplists upgrade: building skiplists, %d%% done\r", iDone );
		}
	}
	fprintf ( stdout, "skiplists upgrade: building skiplists, 100%% done\n" );

	// finalize
	wrSkips.CloseFile ();
	if ( wrSkips.IsError() )
		sphDie ( "skiplists upgrade: write error (out of space?)" );
	if ( rdDocs.GetErrorFlag() )
		sphDie ( "skiplists upgrade: doclist read error: %s", rdDocs.GetErrorMessage().cstr() );

	// sort by entry id again
	dSkips.Sort ( sphMemberLess ( &EntrySkips_t::m_uEntry ) );

	/////////////////////////////
	// write new dictionary file
	/////////////////////////////

	// converted dict writer
	CSphWriter wrDict;
	sFilename.SetSprintf ( "%s.spi.upgrade", sPath );
	if ( !wrDict.OpenFile ( sFilename, sError ) )
		sphDie ( "skiplists upgrade: failed to create %s", sFilename.cstr() );
	wrDict.PutByte ( 1 );

	// handy entry iterator
	// we will use this one to decode entries, and rdDict for other raw access
	pReader->Setup ( &rdDict, iWordsEnd, tIndexSettings.m_eHitless, bWordDict, &g_tThrottle, uVersion>=31 );

	// we have to adjust some of the entries
	// thus we also have to recompute the offset in the checkpoints too
	//
	// infix hashes (if any) in dict=keywords refer to checkpoints by numbers
	// so infix data can simply be copied around

	// new checkpoints
	CSphVector<CSphWordlistCheckpoint> dNewCP;
	int iLastCheckpoint = 0;

	// skiplist lookup
	EntrySkips_t * pSkips = dSkips.Begin();

	// dict encoder state
	SphWordID_t uLastWordid = 0; // crc case
	SphOffset_t iLastDoclist = 0; // crc case
	CSphKeywordDeltaWriter tLastKeyword; // keywords case
	DWORD uWordCount = 0;

	// read old entries, write new entries
	while ( pReader->Read() )
	{
		// update or regenerate checkpoint
		if ( ( !bConvertCheckpoints && iLastCheckpoint!=pReader->GetCheckpoint() )
			|| ( bConvertCheckpoints && ( uWordCount % SPH_WORDLIST_CHECKPOINT )==0 ) )
		{
			// FIXME? GetCheckpoint() is for some reason 1-based
			if ( uWordCount )
			{
				wrDict.ZipInt ( 0 );
				if ( bWordDict )
					wrDict.ZipInt ( 0 );
				else
					wrDict.ZipOffset ( pReader->m_iDoclistOffset - iLastDoclist );
			}
			uLastWordid = 0;
			iLastDoclist = 0;

			CSphWordlistCheckpoint & tCP = dNewCP.Add();
			if ( bWordDict )
			{
				tCP.m_sWord = strdup ( (const char*)pReader->GetWord() );
				tLastKeyword.Reset();
			} else
			{
				tCP.m_iWordID = pReader->m_iWordID;
			}
			tCP.m_iWordlistOffset = wrDict.GetPos();
			iLastCheckpoint = pReader->GetCheckpoint();
		}

		// resave entry
		if ( bWordDict )
		{
			// keywords dict path
			const int iLen = strlen ( (const char*)pReader->GetWord() );
			tLastKeyword.PutDelta ( wrDict, pReader->GetWord(), iLen );
			wrDict.ZipOffset ( pReader->m_iDoclistOffset );
			wrDict.ZipInt ( pReader->m_iDocs );
			wrDict.ZipInt ( pReader->m_iHits );
			if ( pReader->m_iDocs>=DOCLIST_HINT_THRESH )
				wrDict.PutByte ( pReader->m_iHint );
		} else
		{
			// crc dict path
			assert ( pReader->m_iWordID > uLastWordid );
			assert ( pReader->m_iDoclistOffset > iLastDoclist );
			wrDict.ZipOffset ( pReader->m_iWordID - uLastWordid );
			wrDict.ZipOffset ( pReader->m_iDoclistOffset - iLastDoclist );
			wrDict.ZipInt ( pReader->m_iDocs );
			wrDict.ZipInt ( pReader->m_iHits );
			uLastWordid = pReader->m_iWordID;
			iLastDoclist = pReader->m_iDoclistOffset;
		}

		// emit skiplist pointer
		if ( pReader->m_iDocs > SPH_SKIPLIST_BLOCK )
		{
			// lots of checks
			if ( uWordCount!=pSkips->m_uEntry )
				sphDie ( "skiplist upgrade: internal error, entry mismatch (expected %d, got %d)",
					uWordCount, pSkips->m_uEntry );
			if ( pReader->m_iDoclistOffset!=pSkips->m_iDoclist )
				sphDie ( "skiplist upgrade: internal error, offset mismatch (expected %lld, got %lld)",
					INT64 ( pReader->m_iDoclistOffset ), INT64 ( pSkips->m_iDoclist ) );
			if ( pSkips->m_iSkiplist<0 )
				sphDie ( "skiplist upgrade: internal error, bad skiplist offset %d",
					pSkips->m_iSkiplist	);

			// and a bit of work
			wrDict.ZipInt ( pSkips->m_iSkiplist );
			pSkips++;
		}

		// next entry
		uWordCount++;
	}

	// finalize last keywords block
	wrDict.ZipInt ( 0 );
	if ( bWordDict )
		wrDict.ZipInt ( 0 );
	else
		wrDict.ZipOffset ( rdDocs.GetFilesize() - iLastDoclist );

	rdDocs.Close();
	SafeDelete ( pReader );

	// copy infix hash entries, if any
	int iDeltaInfix = 0;
	if ( bWordDict && tDictHeader.m_iInfixCodepointBytes )
	{
		if ( iWordsEnd!=rdDict.GetPos() )
			sphDie ( "skiplist upgrade: internal error, infix hash position mismatch (expected=%lld, got=%lld)",
				INT64 ( iWordsEnd ), INT64 ( rdDict.GetPos() ) );
		iDeltaInfix = (int)( wrDict.GetPos() - rdDict.GetPos() );
		CopyBytes ( wrDict, rdDict, (int)( tDictHeader.m_iDictCheckpointsOffset - iWordsEnd ) );
	}

	// write new checkpoints
	if ( tDictHeader.m_iDictCheckpointsOffset!=rdDict.GetPos() )
		sphDie ( "skiplist upgrade: internal error, checkpoints position mismatch (expected=%lld, got=%lld)",
			INT64 ( tDictHeader.m_iDictCheckpointsOffset ), INT64 ( rdDict.GetPos() ) );
	if ( !bConvertCheckpoints && tDictHeader.m_iDictCheckpoints!=dNewCP.GetLength() )
		sphDie ( "skiplist upgrade: internal error, checkpoint count mismatch (old=%d, new=%d)",
			tDictHeader.m_iDictCheckpoints, dNewCP.GetLength() );

	tDictHeader.m_iDictCheckpoints = dNewCP.GetLength();
	tDictHeader.m_iDictCheckpointsOffset = wrDict.GetPos();
	ARRAY_FOREACH ( i, dNewCP )
	{
		if ( bWordDict )
		{
			wrDict.PutString ( dNewCP[i].m_sWord );
			SafeDeleteArray ( dNewCP[i].m_sWord );
		} else
		{
			wrDict.PutOffset ( dNewCP[i].m_iWordID );
		}
		wrDict.PutOffset ( dNewCP[i].m_iWordlistOffset );
	}

	// update infix hash blocks, if any
	// (they store direct offsets to infix hash, which just got moved)
	if ( bWordDict && tDictHeader.m_iInfixCodepointBytes )
	{
		rdDict.SeekTo ( tDictHeader.m_iInfixBlocksOffset, READ_NO_SIZE_HINT );
		int iBlocks = rdDict.UnzipInt();

		wrDict.PutBytes ( g_sTagInfixBlocks, strlen ( g_sTagInfixBlocks ) );
		tDictHeader.m_iInfixBlocksOffset = (int)wrDict.GetPos();

		wrDict.ZipInt ( iBlocks );
		for ( int i=0; i<iBlocks; i++ )
		{
			char sInfix[256];
			int iBytes = rdDict.GetByte();
			rdDict.GetBytes ( sInfix, iBytes );
			wrDict.PutByte ( iBytes );
			wrDict.PutBytes ( sInfix, iBytes );
			wrDict.ZipInt ( rdDict.UnzipInt() + iDeltaInfix );
		}
	}

	// emit new aux tail header
	if ( bWordDict )
	{
		wrDict.PutBytes ( "dict-header", 11 );
		wrDict.ZipInt ( tDictHeader.m_iDictCheckpoints );
		wrDict.ZipOffset ( tDictHeader.m_iDictCheckpointsOffset );
		wrDict.ZipInt ( tDictHeader.m_iInfixCodepointBytes );
		wrDict.ZipInt ( tDictHeader.m_iInfixBlocksOffset );
	}

	wrDict.CloseFile();
	if ( wrDict.IsError() )
		sphDie ( "skiplists upgrade: dict write error (out of space?)" );

	rdDict.Close();

	////////////////////
	// build min-max attribute index
	////////////////////

	bool bShuffleAttributes = false;
	if ( uVersion<20 )
	{
		int iStride = DOCINFO_IDSIZE + tSchema.GetRowSize();
		int iEntrySize = sizeof(DWORD)*iStride;

		sFilename.SetSprintf ( "%s.spa", sPath );
		CSphAutofile rdDocinfo ( sFilename.cstr(), SPH_O_READ, sError );
		if ( rdDocinfo.GetFD()<0 )
			sphDie ( "skiplists upgrade: %s", sError.cstr() );

		sFilename.SetSprintf ( "%s.spa.upgrade", sPath );
		CSphWriter wrDocinfo;
		if ( !wrDocinfo.OpenFile ( sFilename.cstr(), sError ) )
			sphDie ( "skiplists upgrade: %s", sError.cstr() );

		CSphFixedVector<DWORD> dMva ( 0 );
		CSphAutofile tMvaFile ( sFilename.cstr(), SPH_O_READ, sError );
		if ( tMvaFile.GetFD()>=0 && tMvaFile.GetSize()>0 )
		{
			uint64_t uMvaSize = tMvaFile.GetSize();
			assert ( uMvaSize/sizeof(DWORD)<=UINT_MAX );
			dMva.Reset ( (int)( uMvaSize/sizeof(DWORD) ) );
			tMvaFile.Read ( dMva.Begin(), uMvaSize, sError );
		}
		tMvaFile.Close();

		int64_t iDocinfoSize = rdDocinfo.GetSize ( iEntrySize, true, sError ) / sizeof(CSphRowitem);
		assert ( iDocinfoSize / iStride < UINT_MAX );
		int iRows = (int)(iDocinfoSize/iStride);

		AttrIndexBuilder_c tBuilder ( tSchema );
		int64_t iMinMaxSize = tBuilder.GetExpectedSize ( tStats.m_iTotalDocuments );
		if ( iMinMaxSize>INT_MAX )
			sphDie ( "attribute files (.spa) over 128 GB are not supported" );
		CSphFixedVector<CSphRowitem> dMinMax ( (int)iMinMaxSize );
		tBuilder.Prepare ( dMinMax.Begin(), dMinMax.Begin() + dMinMax.GetLength() ); // FIXME!!! for over INT_MAX blocks

		CSphFixedVector<CSphRowitem> dRow ( iStride );

		uMinMaxIndex = 0;
		for ( int i=0; i<iRows; i++ )
		{
			rdDocinfo.Read ( dRow.Begin(), iStride*sizeof(CSphRowitem), sError );
			wrDocinfo.PutBytes ( dRow.Begin(), iStride*sizeof(CSphRowitem) );

			if ( !tBuilder.Collect ( dRow.Begin(), dMva.Begin(), dMva.GetLength(), sError, true ) )
				sphDie ( "skiplists upgrade: %s", sError.cstr() );

			uMinMaxIndex += iStride;

			int iDone1 = ( 1+i ) * 100 / iRows;
			int iDone2 = ( 2+i ) * 100 / iRows;
			if ( iDone1!=iDone2 )
				fprintf ( stdout, "skiplists upgrade: building attribute min-max, %d%% done\r", iDone1 );
		}
		fprintf ( stdout, "skiplists upgrade: building attribute min-max, 100%% done\n" );

		tBuilder.FinishCollect();
		rdDocinfo.Close();

		wrDocinfo.PutBytes ( dMinMax.Begin(), dMinMax.GetLength()*sizeof(CSphRowitem) );
		wrDocinfo.CloseFile();
		if ( wrDocinfo.IsError() )
			sphDie ( "skiplists upgrade: attribute write error (out of space?)" );

		bShuffleAttributes = true;
	}


	////////////////////
	// write new header
	////////////////////

	ISphTokenizer * pTokenizer = ISphTokenizer::Create ( tTokenizerSettings, &tEmbeddedFiles, sError );
	if ( !pTokenizer )
		sphDie ( "skiplists upgrade: %s", sError.cstr() );

	CSphDict * pDict = bWordDict
		? sphCreateDictionaryKeywords ( tDictSettings, &tEmbeddedFiles, pTokenizer, "$indexname", sError )
		: sphCreateDictionaryCRC ( tDictSettings, &tEmbeddedFiles, pTokenizer, "$indexname", sError );
	if ( !pDict )
		sphDie ( "skiplists upgrade: %s", sError.cstr() );

	CSphWriter wrHeader;
	sFilename.SetSprintf ( "%s.sph.upgrade", sPath );
	if ( !wrHeader.OpenFile ( sFilename, sError ) )
		sphDie ( "skiplists upgrade: %s", sError.cstr() );

	wrHeader.PutDword ( INDEX_MAGIC_HEADER );
	wrHeader.PutDword ( INDEX_FORMAT_VERSION );
	wrHeader.PutDword ( bUse64 );
	wrHeader.PutDword ( eDocinfo );
	WriteSchema ( wrHeader, tSchema );
	wrHeader.PutOffset ( iMinDocid );
	wrHeader.PutOffset ( tDictHeader.m_iDictCheckpointsOffset );
	wrHeader.PutDword ( tDictHeader.m_iDictCheckpoints );
	wrHeader.PutByte ( tDictHeader.m_iInfixCodepointBytes );
	wrHeader.PutDword ( tDictHeader.m_iInfixBlocksOffset );
	wrHeader.PutDword ( tDictHeader.m_iInfixBlocksWordsSize );
	wrHeader.PutDword ( (DWORD)tStats.m_iTotalDocuments ); // FIXME? we don't expect over 4G docs per just 1 local index
	wrHeader.PutOffset ( tStats.m_iTotalBytes );
	SaveIndexSettings ( wrHeader, tIndexSettings );
	SaveTokenizerSettings ( wrHeader, pTokenizer, tIndexSettings.m_iEmbeddedLimit );
	SaveDictionarySettings ( wrHeader, pDict, false, tIndexSettings.m_iEmbeddedLimit );
	wrHeader.PutDword ( iKillListSize );
	wrHeader.PutOffset ( uMinMaxIndex );
	SaveFieldFilterSettings ( wrHeader, pFieldFilter );

	// average field lengths
	if ( tIndexSettings.m_bIndexFieldLens )
		ARRAY_FOREACH ( i, tSchema.m_dFields )
			wrHeader.PutOffset ( dFieldLens[i] );

	wrHeader.CloseFile ();
	if ( wrHeader.IsError() )
		sphDie ( "skiplists upgrade: header write error (out of space?)" );

	sFilename.SetSprintf ( "%s.sps", sPath );
	if ( !sphIsReadable ( sFilename.cstr(), NULL ) )
	{
		CSphWriter wrStrings;
		if ( !wrStrings.OpenFile ( sFilename, sError ) )
			sphDie ( "skiplists upgrade: %s", sError.cstr() );

		wrStrings.PutByte ( 0 );
		wrStrings.CloseFile();
		if ( wrStrings.IsError() )
			sphDie ( "skiplists upgrade: string write error (out of space?)" );
	}

	// all done!
	const char * sRenames[] = {
		".spe.tmp", ".spe",
		".sph", ".sph.bak",
		".spi", ".spi.bak",
		".sph.upgrade", ".sph",
		".spi.upgrade", ".spi",
		bShuffleAttributes ? ".spa" : NULL, ".spa.bak",
		".spa.upgrade", ".spa",
		NULL };
	FinalizeUpgrade ( sRenames, "skiplists upgrade", sPath, tmStart );
}


bool CSphGlobalIDF::Touch ( const CSphString & sFilename )
{
	// update m_uMTime, return true if modified
	struct_stat tStat;
	memset ( &tStat, 0, sizeof ( tStat ) );
	if ( stat ( sFilename.cstr(), &tStat ) < 0 )
		memset ( &tStat, 0, sizeof ( tStat ) );
	bool bModified = ( m_uMTime!=tStat.st_mtime );
	m_uMTime = tStat.st_mtime;
	return bModified;
}


bool CSphGlobalIDF::Preread ( const CSphString & sFilename, CSphString & sError )
{
	Touch ( sFilename );

	CSphAutoreader tReader;
	if ( !tReader.Open ( sFilename, sError ) )
		return false;

	m_iTotalDocuments = tReader.GetOffset ();
	const SphOffset_t iSize = tReader.GetFilesize () - sizeof(SphOffset_t);
	m_iTotalWords = iSize/sizeof(IDFWord_t);

	// allocate words cache
	CSphString sWarning;
	if ( !m_pWords.Alloc ( m_iTotalWords, sError, sWarning ) )
		return false;

	// allocate lookup table if needed
	int iHashSize = (int)( U64C(1) << HASH_BITS );
	if ( m_iTotalWords > iHashSize*8 )
	{
		if ( !m_pHash.Alloc ( iHashSize+2, sError, sWarning ) )
			return false;
	}

	// read file into memory (may exceed 2GB)
	const int iBlockSize = 10485760; // 10M block
	for ( SphOffset_t iRead=0; iRead<iSize && !sphInterrupted(); iRead+=iBlockSize )
		tReader.GetBytes ( (BYTE*)m_pWords.GetWritePtr()+iRead, iRead+iBlockSize>iSize ? (int)( iSize-iRead ) : iBlockSize );

	if ( sphInterrupted() )
		return false;

	// build lookup table
	if ( m_pHash.GetLengthBytes () )
	{
		int64_t * pHash = m_pHash.GetWritePtr();

		uint64_t uFirst = m_pWords[0].m_uWordID;
		uint64_t uRange = m_pWords[m_iTotalWords-1].m_uWordID - uFirst;

		DWORD iShift = 0;
		while ( uRange>=( U64C(1) << HASH_BITS ) )
		{
			iShift++;
			uRange >>= 1;
		}

		pHash[0] = iShift;
		pHash[1] = 0;
		DWORD uLastHash = 0;

		for ( int64_t i=1; i<m_iTotalWords; i++ )
		{
			// check for interrupt (throttled for speed)
			if ( ( i & 0xffff )==0 && sphInterrupted() )
				return false;

			DWORD uHash = (DWORD)( ( m_pWords[i].m_uWordID-uFirst ) >> iShift );

			if ( uHash==uLastHash )
				continue;

			while ( uLastHash<uHash )
				pHash [ ++uLastHash+1 ] = i;

			uLastHash = uHash;
		}
		pHash [ ++uLastHash+1 ] = m_iTotalWords;
	}
	return true;
}


DWORD CSphGlobalIDF::GetDocs ( const CSphString & sWord ) const
{
	const char * s = sWord.cstr();

	// replace = to MAGIC_WORD_HEAD_NONSTEMMED for exact terms
	char sBuf [ 3*SPH_MAX_WORD_LEN+4 ];
	if ( *s && *s=='=' )
	{
		strncpy ( sBuf, sWord.cstr(), sizeof(sBuf) );
		sBuf[0] = MAGIC_WORD_HEAD_NONSTEMMED;
		s = sBuf;
	}

	uint64_t uWordID = sphFNV64 ( (const BYTE *)s );

	int64_t iStart = 0;
	int64_t iEnd = m_iTotalWords-1;

	const IDFWord_t * pWords = (IDFWord_t *)m_pWords.GetWritePtr ();

	if ( m_pHash.GetLengthBytes () )
	{
		uint64_t uFirst = pWords[0].m_uWordID;
		DWORD uHash = (DWORD)( ( uWordID-uFirst ) >> m_pHash[0] );
		if ( uHash > ( U64C(1) << HASH_BITS ) )
			return 0;

		iStart = m_pHash [ uHash+1 ];
		iEnd = m_pHash [ uHash+2 ] - 1;
	}

	const IDFWord_t * pWord = sphBinarySearch ( pWords+iStart, pWords+iEnd, bind ( &IDFWord_t::m_uWordID ), uWordID );
	return pWord ? pWord->m_iDocs : 0;
}


float CSphGlobalIDF::GetIDF ( const CSphString & sWord, int64_t iDocsLocal, bool bPlainIDF )
{
	const int64_t iDocs = Max ( iDocsLocal, (int64_t)GetDocs ( sWord ) );
	const int64_t iTotalClamped = Max ( m_iTotalDocuments, iDocs );

	if ( !iDocs )
		return 0.0f;

	if ( bPlainIDF )
	{
		float fLogTotal = logf ( float ( 1+iTotalClamped ) );
		return logf ( float ( iTotalClamped-iDocs+1 ) / float ( iDocs ) )
			/ ( 2*fLogTotal );
	} else
	{
		float fLogTotal = logf ( float ( 1+iTotalClamped ) );
		return logf ( float ( iTotalClamped ) / float ( iDocs ) )
			/ ( 2*fLogTotal );
	}
}


bool sphPrereadGlobalIDF ( const CSphString & sPath, CSphString & sError )
{
	g_tGlobalIDFLock.Lock ();

	CSphGlobalIDF ** ppGlobalIDF = g_hGlobalIDFs ( sPath );
	bool bExpired = ( ppGlobalIDF && *ppGlobalIDF && (*ppGlobalIDF)->Touch ( sPath ) );

	if ( !ppGlobalIDF || bExpired )
	{
		if ( bExpired )
			sphLogDebug ( "Reloading global IDF (%s)", sPath.cstr() );
		else
			sphLogDebug ( "Loading global IDF (%s)", sPath.cstr() );

		// unlock while prereading
		g_tGlobalIDFLock.Unlock ();

		CSphGlobalIDF * pGlobalIDF = new CSphGlobalIDF ();
		if ( !pGlobalIDF->Preread ( sPath, sError ) )
		{
			SafeDelete ( pGlobalIDF );
			return false;
		}

		// lock while updating
		g_tGlobalIDFLock.Lock ();

		if ( bExpired )
		{
			ppGlobalIDF = g_hGlobalIDFs ( sPath );
			if ( ppGlobalIDF )
			{
				CSphGlobalIDF * pOld = *ppGlobalIDF;
				*ppGlobalIDF = pGlobalIDF;
				SafeDelete ( pOld );
			}
		} else
		{
			if ( !g_hGlobalIDFs.Add ( pGlobalIDF, sPath ) )
				SafeDelete ( pGlobalIDF );
		}
	}

	g_tGlobalIDFLock.Unlock ();

	return true;
}


void sphUpdateGlobalIDFs ( const CSphVector<CSphString> & dFiles )
{
	// delete unlisted entries
	g_tGlobalIDFLock.Lock ();
	g_hGlobalIDFs.IterateStart ();
	while ( g_hGlobalIDFs.IterateNext () )
	{
		const CSphString & sKey = g_hGlobalIDFs.IterateGetKey ();
		if ( !dFiles.Contains ( sKey ) )
		{
			sphLogDebug ( "Unloading global IDF (%s)", sKey.cstr() );
			SafeDelete ( g_hGlobalIDFs.IterateGet () );
			g_hGlobalIDFs.Delete ( sKey );
		}
	}
	g_tGlobalIDFLock.Unlock ();

	// load/rotate remaining entries
	CSphString sError;
	ARRAY_FOREACH ( i, dFiles )
	{
		CSphString sPath = dFiles[i];
		if ( !sphPrereadGlobalIDF ( sPath, sError ) )
			sphLogDebug ( "Could not load global IDF (%s): %s", sPath.cstr(), sError.cstr() );
	}
}


void sphShutdownGlobalIDFs ()
{
	CSphVector<CSphString> dEmptyFiles;
	sphUpdateGlobalIDFs ( dEmptyFiles );
}

//////////////////////////////////////////////////////////////////////////

//
// $Id: sphinx.cpp 4310M 2013-11-09 05:22:08Z (local) $
//
