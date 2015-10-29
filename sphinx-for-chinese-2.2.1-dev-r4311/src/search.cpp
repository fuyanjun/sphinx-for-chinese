//
// $Id: search.cpp 4267 2013-10-18 08:14:50Z tomat $
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

#include "sphinx.h"
#include "sphinxutils.h"
#include "sphinxint.h"
#include <time.h>


#define CONF_CHECK(_hash,_key,_msg,_add) \
	if (!( _hash.Exists ( _key ) )) \
	{ \
		fprintf ( stdout, "ERROR: key '%s' not found " _msg, _key, _add ); \
		continue; \
	}


const char * myctime ( DWORD uStamp )
{
	static char sBuf[256];
	time_t tStamp = uStamp; // for 64-bit
	strncpy ( sBuf, ctime ( &tStamp ), sizeof(sBuf) );

	char * p = sBuf;
	while ( (*p) && (*p)!='\n' && (*p)!='\r' ) p++;
	*p = '\0';

	return sBuf;
}


int main ( int argc, char ** argv )
{
	fprintf ( stdout, SPHINX_BANNER );
	if ( argc<=1 )
	{
		fprintf ( stdout,
			"Usage: search [OPTIONS] <word1 [word2 [word3 [...]]]>\n"
			"\n"
			"Options are:\n"
			"-c, --config <file>\tuse given config file instead of defaults\n"
			"-i, --index <index>\tsearch given index only (default: all indexes)\n"
			"-a, --any\t\tmatch any query word (default: match all words)\n"
			"-b, --boolean\t\tmatch in boolean mode\n"
			"-p, --phrase\t\tmatch exact phrase\n"
			"-e, --extended\t\tmatch in extended mode\n"
			"-f, --filter <attr> <v>\tonly match if attribute attr value is v\n"
			"-s, --sortby <CLAUSE>\tsort matches by 'CLAUSE' in sort_extended mode\n"
			"-S, --sortexpr <EXPR>\tsort matches by 'EXPR' DESC in sort_expr mode\n"
			"-o, --offset <offset>\tprint matches starting from this offset (default: 0)\n"
			"-l, --limit <count>\tprint this many matches (default: 20)\n"
			"-q, --noinfo\t\tdon't print document info from SQL database\n"
			"-g, --group <attr>\tgroup by attribute named attr\n"
			"-gs,--groupsort <expr>\tsort groups by <expr>\n"
			"--sort=date\t\tsort by date, descending\n"
			"--rsort=date\t\tsort by date, ascending\n"
			"--sort=ts\t\tsort by time segments\n"
			"--stdin\t\t\tread query from stdin\n"
			"\n"
			"This program (CLI search) is for testing and debugging purposes only;\n"
			"it is NOT intended for production use.\n"
		);
		exit ( 0 );
	}

	///////////////////////////////////////////
	// get query and other commandline options
	///////////////////////////////////////////

	CSphQuery tQuery;
	char sQuery [ 1024 ];
	sQuery[0] = '\0';

	const char * sOptConfig = NULL;
	const char * sIndex = NULL;
	bool bNoInfo = false;
	bool bStdin = false;
	int iStart = 0;
	int iLimit = 20;

	#define OPT(_a1,_a2)	else if ( !strcmp(argv[i],_a1) || !strcmp(argv[i],_a2) )
	#define OPT1(_a1)		else if ( !strcmp(argv[i],_a1) )

	int i;
	for ( i=1; i<argc; i++ )
	{
		if ( argv[i][0]=='-' )
		{
			// this is an option
			if ( i==0 );
			OPT ( "-a", "--any" )		tQuery.m_eMode = SPH_MATCH_ANY;
			OPT ( "-b", "--boolean" )	tQuery.m_eMode = SPH_MATCH_BOOLEAN;
			OPT ( "-p", "--phrase" )	tQuery.m_eMode = SPH_MATCH_PHRASE;
			OPT ( "-e", "--ext" )		tQuery.m_eMode = SPH_MATCH_EXTENDED;
			OPT ( "-e2", "--ext2" )		tQuery.m_eMode = SPH_MATCH_EXTENDED2;
			OPT ( "-q", "--noinfo" )	bNoInfo = true;
			OPT1 ( "--sort=date" )		tQuery.m_eSort = SPH_SORT_ATTR_DESC;
			OPT1 ( "--rsort=date" )		tQuery.m_eSort = SPH_SORT_ATTR_ASC;
			OPT1 ( "--sort=ts" )		tQuery.m_eSort = SPH_SORT_TIME_SEGMENTS;
			OPT1 ( "--stdin" )			bStdin = true;

			else if ( (i+1)>=argc )		break;
			OPT ( "-o", "--offset" )	iStart = atoi ( argv[++i] );
			OPT ( "-l", "--limit" )		iLimit = atoi ( argv[++i] );
			OPT ( "-c", "--config" )	sOptConfig = argv[++i];
			OPT ( "-i", "--index" )		sIndex = argv[++i];
			OPT ( "-g", "--group" )		{ tQuery.m_eGroupFunc = SPH_GROUPBY_ATTR; tQuery.m_sGroupBy = argv[++i]; }
			OPT ( "-gs","--groupsort" )	{ tQuery.m_sGroupSortBy = argv[++i]; } // NOLINT
			OPT ( "-s", "--sortby" )	{ tQuery.m_eSort = SPH_SORT_EXTENDED; tQuery.m_sSortBy = argv[++i]; }
			OPT ( "-S", "--sortexpr" )	{ tQuery.m_eSort = SPH_SORT_EXPR; tQuery.m_sSortBy = argv[++i]; }

			else if ( (i+2)>=argc )		break;
			OPT ( "-f", "--filter" )
			{
				DWORD uVal = strtoul ( argv[i+2], NULL, 10 );
				CSphFilterSettings * pFilter = NULL;
				// do we already have a filter for that attribute?
				ARRAY_FOREACH ( j, tQuery.m_dFilters )
				{
					if ( tQuery.m_dFilters[j].m_sAttrName==argv[i+1] )
					{
						pFilter = &tQuery.m_dFilters[j];
						break;
					}
				}
				if ( !pFilter )
				{
					pFilter = &tQuery.m_dFilters.Add ();
					pFilter->m_eType = SPH_FILTER_VALUES;
					pFilter->m_sAttrName = argv[i+1];
				}
				pFilter->m_dValues.Add ( uVal );
				pFilter->m_dValues.Uniq ();
				i += 2;
			} else						break; // unknown option

		} else if ( strlen(sQuery) + strlen(argv[i]) + 1 < sizeof(sQuery) )
		{
			// this is a search term
			strcat ( sQuery, argv[i] ); // NOLINT
			strcat ( sQuery, " " ); // NOLINT
		}
	}
	iStart = Max ( iStart, 0 );
	iLimit = Max ( iLimit, 0 );

	if ( i!=argc )
	{
		fprintf ( stdout, "ERROR: malformed or unknown option near '%s'.\n", argv[i] );
		return 1;
	}

	#undef OPT

	tzset();

	if ( bStdin )
	{
		int iPos = 0, iLeft = sizeof(sQuery)-1;
		char sThrowaway [ 256 ];

		while ( !feof(stdin) )
		{
			if ( iLeft>0 )
			{
				int iLen = fread ( sQuery, 1, iLeft, stdin );
				iPos += iLen;
				iLeft -= iLen;
			} else
			{
				int iDummy; // to avoid gcc unused result warning
				iDummy = fread ( sThrowaway, 1, sizeof(sThrowaway), stdin );
				iDummy++; // to avoid gcc set but not used warning
			}
		}

		assert ( iPos<(int)sizeof(sQuery) );
		sQuery[iPos] = '\0';
	}

	/////////////
	// configure
	/////////////

	tQuery.m_iMaxMatches = Max ( 1000, iStart + iLimit );

	CSphConfigParser cp;
	CSphConfig & hConf = cp.m_tConf;
	sphLoadConfig ( sOptConfig, false, cp );

	/////////////////////
	// search each index
	/////////////////////

	hConf["index"].IterateStart ();
	while ( hConf["index"].IterateNext () )
	{
		const CSphConfigSection & hIndex = hConf["index"].IterateGet ();
		const char * sIndexName = hConf["index"].IterateGetKey().cstr();

		if ( sIndex && strcmp ( sIndex, sIndexName ) )
			continue;

		if ( hIndex("type") && hIndex["type"]=="distributed" )
			continue;

		if ( !hIndex.Exists ( "path" ) )
			sphDie ( "key 'path' not found in index '%s'", sIndexName );

		CSphString sError;

		// do we want to show document info from database?
		#if USE_MYSQL
		MYSQL tSqlDriver;
		const char * sQueryInfo = NULL;

		while ( !bNoInfo )
		{
			if ( !hIndex("source") || !hConf("source") || !hConf["source"]( hIndex["source"] ) )
				break;

			const CSphConfigSection & hSource = hConf["source"][ hIndex["source"] ];
			if ( !hSource("type") || hSource["type"]!="mysql"
				|| !hSource("sql_host") || !hSource("sql_user") || !hSource("sql_db") || !hSource("sql_pass") || !hSource("sql_query_info") )
			{
				break;
			}

			sQueryInfo = hSource["sql_query_info"].cstr();
			if ( !strstr ( sQueryInfo, "$id" ) )
				sphDie ( "'sql_query_info' value must contain '$id'" );

			int iPort = 3306;
			if ( hSource.Exists ( "sql_port" ) && hSource["sql_port"].intval() )
				iPort = hSource["sql_port"].intval();

			mysql_init ( &tSqlDriver );
			if ( !mysql_real_connect ( &tSqlDriver,
				hSource["sql_host"].cstr(),
				hSource["sql_user"].cstr(),
				hSource["sql_pass"].cstr(),
				hSource["sql_db"].cstr(),
				iPort,
				hSource.Exists ( "sql_sock" ) ? hSource["sql_sock"].cstr() : NULL,
				0 ) )
			{
				sphDie ( "failed to connect to MySQL (error=%s)", mysql_error ( &tSqlDriver ) );
			}

			// all good
			break;
		}
		#endif

		//////////
		// search
		//////////

		tQuery.m_sQuery = sQuery;
		CSphQueryResult * pResult = NULL;

		CSphIndex * pIndex = sphCreateIndexPhrase ( sIndexName, hIndex["path"].cstr() );
		pIndex->SetEnableStar ( hIndex.GetInt("enable_star")!=0 );
		pIndex->SetWordlistPreload ( hIndex.GetInt("ondisk_dict")==0 );
		pIndex->SetGlobalIDFPath ( hIndex.GetStr ( "global_idf" ) );

		CSphString sWarning;

		sError = "could not create index (check that files exist)";
		for ( ; pIndex; )
		{
			if ( !pIndex->Prealloc ( false, false, sWarning ) || !pIndex->Preread() )
			{
				sError = pIndex->GetLastError ();
				break;
			}
			const CSphSchema * pSchema = &pIndex->GetMatchSchema();

			if ( !sWarning.IsEmpty () )
				fprintf ( stdout, "WARNING: index '%s': %s\n", sIndexName, sWarning.cstr () );

			// handle older index versions (<9)
			if ( !sphFixupIndexSettings ( pIndex, hIndex, sError ) )
				sphDie ( "index '%s': %s", sIndexName, sError.cstr() );

			if ( hIndex ( "global_idf" ) && !sphPrereadGlobalIDF ( hIndex.GetStr ( "global_idf" ), sError ) )
				sphDie ( "index '%s': %s", sIndexName, sError.cstr() );

			// lookup first timestamp if needed
			// FIXME! remove this?
			if ( tQuery.m_eSort!=SPH_SORT_RELEVANCE && tQuery.m_eSort!=SPH_SORT_EXTENDED && tQuery.m_eSort!=SPH_SORT_EXPR )
			{
				int iTS = -1;
				for ( int i=0; i<pSchema->GetAttrsCount(); i++ )
					if ( pSchema->GetAttr(i).m_eAttrType==SPH_ATTR_TIMESTAMP )
				{
					tQuery.m_sSortBy = pSchema->GetAttr(i).m_sName;
					iTS = i;
					break;
				}
				if ( iTS<0 )
				{
					fprintf ( stdout, "index '%s': no timestamp attributes found, sorting by relevance.\n", sIndexName );
					tQuery.m_eSort = SPH_SORT_RELEVANCE;
				}
			}

			// do querying
			SphQueueSettings_t tQueueSettings ( tQuery, pIndex->GetMatchSchema(), sError, NULL );
			ISphMatchSorter * pTop = sphCreateQueue ( tQueueSettings );
			if ( !pTop )
			{
				sError.SetSprintf ( "failed to create sorting queue: %s", sError.cstr() );
				break;
			}

			CSphMultiQueryArgs tArgs ( NULL, 1 );
			pResult = new CSphQueryResult();
			if ( !pIndex->MultiQuery ( &tQuery, pResult, 1, &pTop, tArgs ) )
			{
				// failure; pull that error message
				sError = pResult->m_sError;
				SafeDelete ( pResult );
			} else
			{
				// success; fold them matches
				pResult->m_dMatches.Reset ();
				pResult->m_iTotalMatches += pTop->GetTotalCount();
				pResult->m_tSchema = pTop->GetSchema();
				sphFlattenQueue ( pTop, pResult, 0 );
			}

			SafeDelete ( pTop );
			break;
		}

		/////////
		// print
		/////////

		if ( !pResult )
		{
			fprintf ( stdout, "index '%s': search error: %s.\n", sIndexName, sError.cstr() );
			return 1;
		}

		fprintf ( stdout, "index '%s': query '%s': returned %d matches of "INT64_FMT" total in %d.%03d sec\n",
			sIndexName, sQuery, pResult->m_dMatches.GetLength(), pResult->m_iTotalMatches,
			pResult->m_iQueryTime/1000, pResult->m_iQueryTime%1000 );

		if ( !pResult->m_sWarning.IsEmpty() )
			fprintf ( stdout, "WARNING: %s\n", pResult->m_sWarning.cstr() );

		if ( pResult->m_dMatches.GetLength() )
		{
			fprintf ( stdout, "\ndisplaying matches:\n" );

			int iMaxIndex = Min ( iStart+iLimit, pResult->m_dMatches.GetLength() );
			for ( int i=iStart; i<iMaxIndex; i++ )
			{
				CSphMatch & tMatch = pResult->m_dMatches[i];
				fprintf ( stdout, "%d. document=" DOCID_FMT ", weight=%d", 1+i, tMatch.m_iDocID, tMatch.m_iWeight );

				for ( int j=0; j<pResult->m_tSchema.GetAttrsCount(); j++ )
				{
					const CSphColumnInfo & tAttr = pResult->m_tSchema.GetAttr(j);
					fprintf ( stdout, ", %s=", tAttr.m_sName.cstr() );

					if ( tAttr.m_eAttrType==SPH_ATTR_UINT32SET || tAttr.m_eAttrType==SPH_ATTR_INT64SET )
					{
						fprintf ( stdout, "(" );
						SphAttr_t iIndex = tMatch.GetAttr ( tAttr.m_tLocator );
						if ( iIndex )
						{
							const DWORD * pValues = pResult->m_pMva + iIndex;
							int iValues = *pValues++;
							if ( tAttr.m_eAttrType==SPH_ATTR_INT64SET )
							{
								assert ( ( iValues%2 )==0 );
								for ( int k=0; k<iValues; k+=2, pValues+=2 )
								{
									uint64_t uMva = MVA_UPSIZE ( pValues );
									fprintf ( stdout, k ? ","UINT64_FMT : UINT64_FMT, uMva );
								}
							} else
							{
								for ( int k=0; k<iValues; k++ )
									fprintf ( stdout, k ? ",%u" : "%u", *pValues++ );
							}
						}
						fprintf ( stdout, ")" );

					} else switch ( tAttr.m_eAttrType )
					{
						case SPH_ATTR_INTEGER:
						case SPH_ATTR_ORDINAL:
						case SPH_ATTR_BOOL:			fprintf ( stdout, "%u", (DWORD)tMatch.GetAttr ( tAttr.m_tLocator ) ); break;
						case SPH_ATTR_TIMESTAMP:	fprintf ( stdout, "%s", myctime ( (DWORD)tMatch.GetAttr ( tAttr.m_tLocator ) ) ); break;
						case SPH_ATTR_FLOAT:		fprintf ( stdout, "%f", tMatch.GetAttrFloat ( tAttr.m_tLocator ) ); break;
						case SPH_ATTR_BIGINT:		fprintf ( stdout, INT64_FMT, tMatch.GetAttr ( tAttr.m_tLocator ) ); break;
						case SPH_ATTR_STRING:
							{
								const BYTE * pStr;
								int iLen = sphUnpackStr ( pResult->m_pStrings + tMatch.GetAttr ( tAttr.m_tLocator ), &pStr );
								fwrite ( pStr, 1, iLen, stdout );
								break;
							}
						default:					fprintf ( stdout, "(unknown-type-%d)", tAttr.m_eAttrType );
					}
				}
				fprintf ( stdout, "\n" );

				#if USE_MYSQL
				if ( sQueryInfo )
				{
					char * sQuery = sphStrMacro ( sQueryInfo, "$id", tMatch.m_iDocID );
					const char * sError = NULL;

					#define LOC_MYSQL_ERROR(_arg) { sError = _arg; break; }
					for ( ;; )
					{
						if ( mysql_query ( &tSqlDriver, sQuery ) )
							LOC_MYSQL_ERROR ( "mysql_query" );

						MYSQL_RES * pSqlResult = mysql_use_result ( &tSqlDriver );
						if ( !pSqlResult )
							LOC_MYSQL_ERROR ( "mysql_use_result" );

						MYSQL_ROW tRow = mysql_fetch_row ( pSqlResult );
						if ( !tRow )
						{
							fprintf ( stdout, "\t(document not found in db)\n" );
							break;
						}

						for ( int iField=0; iField<(int)pSqlResult->field_count; iField++ )
							fprintf ( stdout, "\t%s=%s\n",
								( pSqlResult->fields && pSqlResult->fields[iField].name ) ? pSqlResult->fields[iField].name : "(NULL)",
								tRow[iField] ? tRow[iField] : "(NULL)" );

						mysql_free_result ( pSqlResult );
						break;
					}

					if ( sError )
						sphDie ( "sql_query_info: %s: %s", sError, mysql_error ( &tSqlDriver ) );

					delete [] sQuery;
				}
				#endif
			}
		}

		fprintf ( stdout, "\nwords:\n" );
		pResult->m_hWordStats.IterateStart();
		int iWord = 1;
		while ( pResult->m_hWordStats.IterateNext() )
		{
			const CSphQueryResultMeta::WordStat_t & tStat = pResult->m_hWordStats.IterateGet();
			fprintf ( stdout, "%d. '%s': "INT64_FMT" documents, "INT64_FMT" hits\n",
				iWord,
				pResult->m_hWordStats.IterateGetKey().cstr(),
				tStat.m_iDocs,
				tStat.m_iHits );
			iWord++;
		}
		fprintf ( stdout, "\n" );

		///////////
		// cleanup
		///////////

		SafeDelete ( pIndex );
	}

	sphShutdownWordforms ();
}

//
// $Id: search.cpp 4267 2013-10-18 08:14:50Z tomat $
//
