/*
 *    Copyright (C) 2010 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

// strategy_sharded.cpp

#include "mongo/pch.h"

#include "mongo/base/status.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/max_time.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/index_details.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/client_info.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/cursors.h"
#include "mongo/s/grid.h"
#include "mongo/s/request.h"
#include "mongo/s/version_manager.h"
#include "mongo/s/write_ops/batch_upconvert.h"
#include "mongo/util/mongoutils/str.h"

// error codes 8010-8040

namespace mongo {

    static bool _isSystemIndexes( const char* ns ) {
        return nsToCollectionSubstring(ns) == "system.indexes";
    }

    void Strategy::queryOp( Request& r ) {

        verify( !NamespaceString( r.getns() ).isCommand() );

        Timer queryTimer;

        QueryMessage q( r.d() );

        NamespaceString ns(q.ns);
        ClientBasic* client = ClientBasic::getCurrent();
        AuthorizationSession* authSession = client->getAuthorizationSession();
        Status status = authSession->checkAuthForQuery(ns, q.query);
        audit::logQueryAuthzCheck(client, ns, q.query, status.code());
        uassertStatusOK(status);

        LOG(3) << "shard query: " << q.ns << "  " << q.query << endl;

        if ( q.ntoreturn == 1 && strstr(q.ns, ".$cmd") )
            throw UserException( 8010 , "something is wrong, shouldn't see a command here" );

        QuerySpec qSpec( (string)q.ns, q.query, q.fields, q.ntoskip, q.ntoreturn, q.queryOptions );

        // Parse "$maxTimeMS".
        StatusWith<int> maxTimeMS = LiteParsedQuery::parseMaxTimeMSQuery( q.query );
        uassert( 17233,
                 maxTimeMS.getStatus().reason(),
                 maxTimeMS.isOK() );

        if ( _isSystemIndexes( q.ns ) && q.query["ns"].type() == String && r.getConfig()->isSharded( q.query["ns"].String() ) ) {
            // if you are querying on system.indexes, we need to make sure we go to a shard that actually has chunks
            // this is not a perfect solution (what if you just look at all indexes)
            // but better than doing nothing

            ShardPtr myShard;
            ChunkManagerPtr cm;
            r.getConfig()->getChunkManagerOrPrimary( q.query["ns"].String(), cm, myShard );
            if ( cm ) {
                set<Shard> shards;
                cm->getAllShards( shards );
                verify( shards.size() > 0 );
                myShard.reset( new Shard( *shards.begin() ) );
            }
            
            doIndexQuery( r, *myShard );
            return;
        }

        ParallelSortClusteredCursor * cursor = new ParallelSortClusteredCursor( qSpec, CommandInfo() );
        verify( cursor );

        // TODO:  Move out to Request itself, not strategy based
        try {
            cursor->init();

            if ( qSpec.isExplain() ) {
                BSONObjBuilder explain_builder;
                cursor->explain( explain_builder );
                explain_builder.appendNumber( "millis",
                                              static_cast<long long>(queryTimer.millis()) );
                BSONObj b = explain_builder.obj();

                replyToQuery( 0 , r.p() , r.m() , b );
                delete( cursor );
                return;
            }
        }
        catch(...) {
            delete cursor;
            throw;
        }

        if( cursor->isSharded() ){
            ShardedClientCursorPtr cc (new ShardedClientCursor( q , cursor ));

            BufBuilder buffer( ShardedClientCursor::INIT_REPLY_BUFFER_SIZE );
            int docCount = 0;
            const int startFrom = cc->getTotalSent();
            bool hasMore = cc->sendNextBatch( r, q.ntoreturn, buffer, docCount );

            if ( hasMore ) {
                LOG(5) << "storing cursor : " << cc->getId() << endl;

                int cursorLeftoverMillis = maxTimeMS.getValue() - queryTimer.millis();
                if ( maxTimeMS.getValue() == 0 ) { // 0 represents "no limit".
                    cursorLeftoverMillis = kMaxTimeCursorNoTimeLimit;
                }
                else if ( cursorLeftoverMillis <= 0 ) {
                    cursorLeftoverMillis = kMaxTimeCursorTimeLimitExpired;
                }

                cursorCache.store( cc, cursorLeftoverMillis );
            }

            replyToQuery( 0, r.p(), r.m(), buffer.buf(), buffer.len(), docCount,
                    startFrom, hasMore ? cc->getId() : 0 );
        }
        else{
            // Remote cursors are stored remotely, we shouldn't need this around.
            // TODO: we should probably just make cursor an auto_ptr
            scoped_ptr<ParallelSortClusteredCursor> cursorDeleter( cursor );

            // TODO:  Better merge this logic.  We potentially can now use the same cursor logic for everything.
            ShardPtr primary = cursor->getPrimary();
            verify( primary.get() );
            DBClientCursorPtr shardCursor = cursor->getShardCursor( *primary );

            // Implicitly stores the cursor in the cache
            r.reply( *(shardCursor->getMessage()) , shardCursor->originalHost() );

            // We don't want to kill the cursor remotely if there's still data left
            shardCursor->decouple();
        }
    }

    void Strategy::doIndexQuery( Request& r , const Shard& shard ) {

        ShardConnection dbcon( shard , r.getns() );
        DBClientBase &c = dbcon.conn();

        string actualServer;

        Message response;
        bool ok = c.call( r.m(), response, true , &actualServer );
        uassert( 10200 , "mongos: error calling db", ok );

        {
            QueryResult *qr = (QueryResult *) response.singleData();
            if ( qr->resultFlags() & ResultFlag_ShardConfigStale ) {
                dbcon.done();
                // Version is zero b/c this is deprecated codepath
                throw RecvStaleConfigException( r.getns() , "Strategy::doQuery", ChunkVersion( 0, OID() ), ChunkVersion( 0, OID() ) );
            }
        }

        r.reply( response , actualServer.size() ? actualServer : c.getServerAddress() );
        dbcon.done();
    }

    void Strategy::clientCommandOp( Request& r ) {
        QueryMessage q( r.d() );

        LOG(3) << "single query: " << q.ns << "  " << q.query << "  ntoreturn: " << q.ntoreturn << " options : " << q.queryOptions << endl;

        NamespaceString nss( r.getns() );
        // Regular queries are handled in strategy_shard.cpp
        verify( nss.isCommand() || nss.isSpecialCommand() );

        if ( handleSpecialNamespaces( r , q ) )
            return;

        int loops = 5;
        while ( true ) {
            BSONObjBuilder builder;
            try {
                BSONObj cmdObj = q.query;
                {
                    BSONElement e = cmdObj.firstElement();
                    if (e.type() == Object && (e.fieldName()[0] == '$'
                                                 ? str::equals("query", e.fieldName()+1)
                                                 : str::equals("query", e.fieldName()))) {
                        // Extract the embedded query object.

                        if (cmdObj.hasField(Query::ReadPrefField.name())) {
                            // The command has a read preference setting. We don't want
                            // to lose this information so we copy this to a new field
                            // called $queryOptions.$readPreference
                            BSONObjBuilder finalCmdObjBuilder;
                            finalCmdObjBuilder.appendElements(e.embeddedObject());

                            BSONObjBuilder queryOptionsBuilder(
                                    finalCmdObjBuilder.subobjStart("$queryOptions"));
                            queryOptionsBuilder.append(cmdObj[Query::ReadPrefField.name()]);
                            queryOptionsBuilder.done();

                            cmdObj = finalCmdObjBuilder.obj();
                        }
                        else {
                            cmdObj = e.embeddedObject();
                        }
                    }
                }

                Command::runAgainstRegistered(q.ns, cmdObj, builder, q.queryOptions);
                BSONObj x = builder.done();
                replyToQuery(0, r.p(), r.m(), x);
                return;
            }
            catch ( StaleConfigException& e ) {
                if ( loops <= 0 )
                    throw e;

                loops--;
                log() << "retrying command: " << q.query << endl;

                // For legacy reasons, ns may not actually be set in the exception :-(
                string staleNS = e.getns();
                if( staleNS.size() == 0 ) staleNS = q.ns;

                ShardConnection::checkMyConnectionVersions( staleNS );
                if( loops < 4 ) versionManager.forceRemoteCheckShardVersionCB( staleNS );
            }
            catch ( AssertionException& e ) {
                Command::appendCommandStatus(builder, e.toStatus());
                BSONObj x = builder.done();
                replyToQuery(0, r.p(), r.m(), x);
                return;
            }
        }
    }

    bool Strategy::handleSpecialNamespaces( Request& r , QueryMessage& q ) {
        const char * ns = strstr( r.getns() , ".$cmd.sys." );
        if ( ! ns )
            return false;
        ns += 10;

        BSONObjBuilder b;
        vector<Shard> shards;

        ClientBasic* client = ClientBasic::getCurrent();
        AuthorizationSession* authSession = client->getAuthorizationSession();
        if ( strcmp( ns , "inprog" ) == 0 ) {
            const bool isAuthorized = authSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(), ActionType::inprog);
            audit::logInProgAuthzCheck(
                    client, q.query, isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
            uassert(ErrorCodes::Unauthorized, "not authorized to run inprog", isAuthorized);

            Shard::getAllShards( shards );

            BSONArrayBuilder arr( b.subarrayStart( "inprog" ) );

            for ( unsigned i=0; i<shards.size(); i++ ) {
                Shard shard = shards[i];
                ScopedDbConnection conn(shard.getConnString());
                BSONObj temp = conn->findOne( r.getns() , q.query );
                if ( temp["inprog"].isABSONObj() ) {
                    BSONObjIterator i( temp["inprog"].Obj() );
                    while ( i.more() ) {
                        BSONObjBuilder x;

                        BSONObjIterator j( i.next().Obj() );
                        while( j.more() ) {
                            BSONElement e = j.next();
                            if ( str::equals( e.fieldName() , "opid" ) ) {
                                stringstream ss;
                                ss << shard.getName() << ':' << e.numberInt();
                                x.append( "opid" , ss.str() );
                            }
                            else if ( str::equals( e.fieldName() , "client" ) ) {
                                x.appendAs( e , "client_s" );
                            }
                            else {
                                x.append( e );
                            }
                        }
                        arr.append( x.obj() );
                    }
                }
                conn.done();
            }

            arr.done();
        }
        else if ( strcmp( ns , "killop" ) == 0 ) {
            const bool isAuthorized = authSession->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(), ActionType::killop);
            audit::logKillOpAuthzCheck(
                    client,
                    q.query,
                    isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
            uassert(ErrorCodes::Unauthorized, "not authorized to run killop", isAuthorized);

            BSONElement e = q.query["op"];
            if ( e.type() != String ) {
                b.append( "err" , "bad op" );
                b.append( e );
            }
            else {
                b.append( e );
                string s = e.String();
                string::size_type i = s.find( ':' );
                if ( i == string::npos ) {
                    b.append( "err" , "bad opid" );
                }
                else {
                    string shard = s.substr( 0 , i );
                    int opid = atoi( s.substr( i + 1 ).c_str() );
                    b.append( "shard" , shard );
                    b.append( "shardid" , opid );

                    log() << "want to kill op: " << e << endl;
                    Shard s(shard);

                    ScopedDbConnection conn(s.getConnString());
                    conn->findOne( r.getns() , BSON( "op" << opid ) );
                    conn.done();
                }
            }
        }
        else if ( strcmp( ns , "unlock" ) == 0 ) {
            b.append( "err" , "can't do unlock through mongos" );
        }
        else {
            warning() << "unknown sys command [" << ns << "]" << endl;
            return false;
        }

        BSONObj x = b.done();
        replyToQuery(0, r.p(), r.m(), x);
        return true;
    }

    void Strategy::commandOp( const string& db,
                              const BSONObj& command,
                              int options,
                              const string& versionedNS,
                              const BSONObj& targetingQuery,
                              vector<CommandResult>* results )
    {

        QuerySpec qSpec(db + ".$cmd", command, BSONObj(), 0, 1, options);

        ParallelSortClusteredCursor cursor( qSpec, CommandInfo( versionedNS, targetingQuery ) );

        // Initialize the cursor
        cursor.init();

        set<Shard> shards;
        cursor.getQueryShards( shards );

        for( set<Shard>::iterator i = shards.begin(), end = shards.end(); i != end; ++i ){
            CommandResult result;
            result.shardTarget = *i;
            string errMsg; // ignored, should never be invalid b/c an exception thrown earlier
            result.target =
                    ConnectionString::parse( cursor.getShardCursor( *i )->originalHost(),
                                             errMsg );
            result.result = cursor.getShardCursor( *i )->peekFirst().getOwned();
            results->push_back( result );
        }

    }

    void Strategy::getMore( Request& r ) {

        Timer getMoreTimer;

        const char *ns = r.getns();

        // TODO:  Handle stale config exceptions here from coll being dropped or sharded during op
        // for now has same semantics as legacy request
        ChunkManagerPtr info = r.getChunkManager();

        //
        // TODO: Cleanup cursor cache, consolidate into single codepath
        //

        int ntoreturn = r.d().pullInt();
        long long id = r.d().pullInt64();
        string host = cursorCache.getRef( id );
        ShardedClientCursorPtr cursor = cursorCache.get( id );
        int cursorMaxTimeMS = cursorCache.getMaxTimeMS( id );

        // Cursor ids should not overlap between sharded and unsharded cursors
        massert( 17012, str::stream() << "duplicate sharded and unsharded cursor id "
                                      << id << " detected for " << ns
                                      << ", duplicated on host " << host,
                 NULL == cursorCache.get( id ).get() || host.empty() );

        ClientBasic* client = ClientBasic::getCurrent();
        NamespaceString nsString(ns);
        AuthorizationSession* authSession = client->getAuthorizationSession();
        Status status = authSession->checkAuthForGetMore( nsString, id );
        audit::logGetMoreAuthzCheck( client, nsString, id, status.code() );
        uassertStatusOK(status);

        if( !host.empty() ){

            LOG(3) << "single getmore: " << ns << endl;

            // we used ScopedDbConnection because we don't get about config versions
            // not deleting data is handled elsewhere
            // and we don't want to call setShardVersion
            ScopedDbConnection conn(host);

            Message response;
            bool ok = conn->callRead( r.m() , response);
            uassert( 10204 , "dbgrid: getmore: error calling db", ok);

            bool hasMore = (response.singleData()->getCursor() != 0);

            if ( !hasMore ) {
                cursorCache.removeRef( id );
            }

            r.reply( response , "" /*conn->getServerAddress() */ );
            conn.done();
            return;
        }
        else if ( cursor ) {

            if ( cursorMaxTimeMS == kMaxTimeCursorTimeLimitExpired ) {
                cursorCache.remove( id );
                uasserted( ErrorCodes::ExceededTimeLimit, "operation exceeded time limit" );
            }

            // TODO: Try to match logic of mongod, where on subsequent getMore() we pull lots more data?
            BufBuilder buffer( ShardedClientCursor::INIT_REPLY_BUFFER_SIZE );
            int docCount = 0;
            const int startFrom = cursor->getTotalSent();
            bool hasMore = cursor->sendNextBatch( r, ntoreturn, buffer, docCount );

            if ( hasMore ) {
                // still more data
                cursor->accessed();

                if ( cursorMaxTimeMS != kMaxTimeCursorNoTimeLimit ) {
                    // Update remaining amount of time in cursor cache.
                    int cursorLeftoverMillis = cursorMaxTimeMS - getMoreTimer.millis();
                    if ( cursorLeftoverMillis <= 0 ) {
                        cursorLeftoverMillis = kMaxTimeCursorTimeLimitExpired;
                    }
                    cursorCache.updateMaxTimeMS( id, cursorLeftoverMillis );
                }
            }
            else {
                // we've exhausted the cursor
                cursorCache.remove( id );
            }

            replyToQuery( 0, r.p(), r.m(), buffer.buf(), buffer.len(), docCount,
                    startFrom, hasMore ? cursor->getId() : 0 );
            return;
        }
        else {

            LOG( 3 ) << "could not find cursor " << id << " in cache for " << ns << endl;

            replyToQuery( ResultFlag_CursorNotFound , r.p() , r.m() , 0 , 0 , 0 );
            return;
        }
    }

    void Strategy::writeOp( int op , Request& r ) {

        // make sure we have a last error
        dassert( lastError.get( false /* don't create */) );

        OwnedPointerVector<BatchedCommandRequest> requestsOwned;
        vector<BatchedCommandRequest*>& requests = requestsOwned.mutableVector();

        msgToBatchRequests( r.m(), &requests );

        for ( vector<BatchedCommandRequest*>::iterator it = requests.begin();
            it != requests.end(); ++it ) {

            // Multiple commands registered to last error as multiple requests
            if ( it != requests.begin() )
                lastError.startRequest( r.m(), lastError.get( false ) );

            BatchedCommandRequest* request = *it;

            // Adjust namespaces for command
            NamespaceString fullNS( request->getNS() );
            string cmdNS = fullNS.getCommandNS();
            // We only pass in collection name to command
            request->setNS( fullNS.coll() );

            BSONObjBuilder builder;
            BSONObj requestBSON = request->toBSON();

            {
                // Disable the last error object for the duration of the write cmd
                LastError::Disabled disableLastError( lastError.get( false ) );
                Command::runAgainstRegistered( cmdNS.c_str(), requestBSON, builder, 0 );
            }

            BatchedCommandResponse response;
            bool parsed = response.parseBSON( builder.done(), NULL );
            (void) parsed; // for compile
            dassert( parsed && response.isValid( NULL ) );

            // Populate the lastError object based on the write
            lastError.get( false )->reset();
            bool hadError = batchErrorToLastError( *request,
                                                   response,
                                                   lastError.get( false ) );

            // Need to specially count inserts
            if ( op == dbInsert ) {
                for( int i = 0; i < response.getN(); ++i )
                    r.gotInsert();
            }

            // If this is an ordered batch and we had a non-write-concern error, we should
            // stop sending.
            if ( request->getOrdered() && hadError )
                break;
        }
    }

    Strategy * STRATEGY = new Strategy();
}
