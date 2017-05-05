#include "lmdb_util.h"
#include <corto/corto.h>

extern corto_threadKey CLMDB_TLS_KEY;

#define TRACE() printf("TRACE: %s, %i\n", __FILE__, __LINE__)

#define SAFE_STRING(str) std::string(str != nullptr ? str : "")

struct MDB_txn_data
{
    MDB_txn *txn;
    int refCount;
};

CLMDB::Cursor::Cursor(Cursor &&c)
{
    this->m_cursor = c.m_cursor;
    this->m_single = c.m_single;
    this->m_hasNext = c.m_hasNext;
    this->m_name = c.m_name;
    this->m_data = c.m_data;

    c.m_single = false;
    c.m_hasNext = false;
    c.m_cursor = nullptr;
    c.m_data.mv_data = nullptr;
    c.m_data.mv_size = 0;
}

CLMDB::Cursor::Cursor() :
    m_cursor(nullptr),
    m_single(false),
    m_hasNext(false)
{
    m_data.mv_data = nullptr;
    m_data.mv_size = 0;
}

CLMDB::Cursor::~Cursor()
{
    if (m_single)
    {
        FreeData(m_data);
    }
    else if (m_cursor != nullptr)
    {
        MDB_txn_data *data = (MDB_txn_data *)corto_threadTlsGet(CLMDB_TLS_KEY);
        MDB_txn *txn = mdb_cursor_txn(m_cursor);

        mdb_cursor_close(m_cursor);
        m_cursor = nullptr;

        if (data != nullptr && data->txn == txn)
        {
            data->refCount--;
            if (data->refCount == 0)
            {
                mdb_txn_commit(data->txn);
                data->txn = nullptr;
            }
        }
        else
        {
            printf("Wrong txn on Tls %p:%p\n", data!= nullptr ? data->txn : 0, txn);
        }
    }
}

bool CLMDB::Cursor::Begin()
{
    bool retVal = false;

    if (m_cursor != nullptr)
    {
        MDB_val key;
        MDB_val data;
        retVal = mdb_cursor_get(m_cursor, &key, &data, MDB_FIRST) == 0;
        m_hasNext = retVal;
    }

    return retVal;
}

CLMDB::Cursor::Data CLMDB::Cursor::GetData()
{
    Data retVal;
    if (m_single == true)
    {
        if (m_hasNext == true)
        {
            retVal.key = m_name;
            retVal.data = m_data.mv_data;
            retVal.size = m_data.mv_size;
        }
    }
    else if (m_cursor != nullptr)
    {
        int rc = 0;
        MDB_val key;
        MDB_val data;

        rc = mdb_cursor_get(m_cursor, &key, &data, MDB_GET_CURRENT);

        if (rc == 0)
        {
            retVal.key = std::string((char*)key.mv_data, key.mv_size);

            retVal.data = data.mv_data;
            retVal.size = data.mv_size;
        }
        else
        {

        }
    }

    return retVal;
}

bool CLMDB::Cursor::HasNext()
{
    bool retVal = m_hasNext;

    return retVal;
}

bool CLMDB::Cursor::Next()
{
    bool retVal = false;
    if (m_single)
    {
        m_hasNext = false;
    }
    else if (m_cursor != nullptr)
    {
        MDB_val key;
        MDB_val data;

        int rc = mdb_cursor_get(m_cursor, &key, &data, MDB_NEXT);

        retVal = rc == 0;
        m_hasNext = retVal;
    }

    return retVal;
}


CLMDB::LMDB_MAP::LMDB_MAP()
{

}

CLMDB::LMDB_MAP::~LMDB_MAP()
{
    std::map<std::string, MDB_env*>::iterator iter;

    while (m_files.empty() == false)
    {
        iter = m_files.begin();
        mdb_env_close(iter->second);
        m_files.erase(iter);
    }
}


/* *****************************
 *  CLMDB
 * *****************************/

std::mutex       CLMDB::m_ebMtx;
CLMDB::BufferMap CLMDB::m_eventBuffer;
CLMDB::LMDB_MAP  CLMDB::m_factory;


void CLMDB::ProcessEvent()
{
    m_ebMtx.lock();
    BufferMap events = std::move(m_eventBuffer);
    m_eventBuffer.reserve(events.size());
    m_ebMtx.unlock();

    for (EventMap::iterator iter = events.begin();
         iter != events.end();
         iter++)
    {
        Event &event = iter->second;
        if (event.m_event == Event::Type::UPDATE)
        {
            SetData(event);
        }
        else if (event.m_event == Event::Type::DELETE)
        {
            SetData(event);
        }
    }
}

void CLMDB::SetData(Event &event)
{
    std::string &path = event.m_path;
    std::string &db = event.m_db;
    std::string &key = event.m_key;
    std::string &data = event.m_data;

    MDB_env *m_env = CLMDB::GetMDB(path);
    MDB_txn *txn = nullptr;
    MDB_dbi dbi = 0;
    MDB_val key_v;
    MDB_val data_v;

    if (m_env != nullptr)
    {
        if (mdb_txn_begin(m_env, nullptr, 0, &txn) == 0)
        {
            if (mdb_dbi_open(txn, db.c_str(), MDB_CREATE, &dbi) == 0)
            {
                key_v.mv_size = key.size();
                key_v.mv_data = (void*)key.data();

                data_v.mv_size = data.size()+1;
                data_v.mv_data = (void*)data.data();

                if (mdb_put(txn, dbi, &key_v, &data_v, 0) == 0)
                {
                    retCode = mdb_txn_commit(txn);
                }
                else
                {
                    mdb_txn_abort(txn);
                }
            }
            else
            {
                mdb_txn_abort(txn);
            }
        }
    }
}

void CLMDB::DelData(Event &event)
{
    std::string &path = event.m_path;
    std::string &db = event.m_db;
    std::string &key = event.m_key;

    MDB_env *m_env = CLMDB::GetMDB(path);
    MDB_txn *txn = nullptr;
    MDB_dbi dbi = 0;
    MDB_val key_v;
    MDB_val data_v;

    if (m_env != nullptr)
    {
        if (mdb_txn_begin(m_env, nullptr, 0, &txn) == 0)
        {
            if (mdb_dbi_open(txn, db.c_str(), MDB_CREATE, &dbi) == 0)
            {
                key_v.mv_size = key.size();
                key_v.mv_data = (void*)key.data();

                if (mdb_del(txn, dbi, &key_v, &data_v) == 0)
                {
                    retCode = mdb_txn_commit(txn);
                }
                else
                {
                    mdb_txn_abort(txn);
                }
            }
            else
            {
                mdb_txn_abort(txn);
            }
        }
    }
}

MDB_env *CLMDB::GetMDB(std::string path)
{
    MDB_env *env = m_factory.m_files[path];

    return env;
}

void CLMDB::GetData(std::string &path, std::string &db, std::string &key, std::string &out)
{
    StrToLower(db);
    StrToLower(key);


    MDB_env *m_env = CLMDB::GetMDB(path);

    if (m_env != nullptr)
    {
        MDB_txn_data *data = (MDB_txn_data*)corto_threadTlsGet(CLMDB_TLS_KEY);

        if (data == nullptr)
        {
            data = (MDB_txn_data*)malloc(sizeof(MDB_txn_data));
            data->txn = nullptr;
            data->refCount = 0;
            corto_threadTlsSet(CLMDB_TLS_KEY, data);
        }

        if (data->txn == nullptr)
        {
            MDB_txn *txn = nullptr;
            if (mdb_txn_begin(m_env, nullptr, MDB_RDONLY, &txn) == 0)
            {
                data->txn = txn;
            }
        }

        if (data->txn != nullptr)
        {
            data->refCount++;
            MDB_dbi dbi = 0;
            if (mdb_dbi_open(data->txn, db.c_str(), 0, &dbi) == 0)
            {
                MDB_val data_v;
                MDB_val key_v;
                key_v.mv_size = key.size();
                key_v.mv_data = (void*)key.data();
                retCode = mdb_get(data->txn, dbi, &key_v, &data_v);

                if (retCode != 0)
                {
                    out.mv_size = 0;
                    out.mv_data = nullptr;
                }
                else
                {
                    out.mv_size = data_v.mv_size;
                    out.mv_data = calloc(out.mv_size+1,1);
                    memcpy(out.mv_data, data_v.mv_data, out.mv_size);
                }
            }

            data->refCount--;

            if (data->refCount == 0)
            {
                mdb_txn_commit(data->txn);
                data->txn = nullptr;
            }
        }
    }
    return retCode;
}
void CLMDB::FreeData(MDB_val &data)
{
    if (data.mv_data != nullptr)
    {
        free (data.mv_data);
        data.mv_data = nullptr;
        data.mv_size = 0;
    }
}
/* *********************************** *
 * PUBLIC
 * *********************************** */
void CLMDB::Initialize(const char *path, uint32_t flags, mdb_mode_t mode, uint64_t map_size)
{
    std::string key = SAFE_STRING(path);

    MDB_env *env = m_factory.m_files[key];

    if (env == nullptr)
    {
        if (mdb_env_create(&env) == 0)
        {
            if (mdb_env_set_mapsize(env, map_size) == 0 &&
                mdb_env_set_maxdbs(env, 32767) == 0)
            {
                if (mdb_env_open(env, path, flags, mode) == 0)
                {
                    m_factory.m_files[key] = env;
                }
                else
                {
                    mdb_env_close(env);
                    env = nullptr;
                }
            }
            else
            {
                mdb_env_close(env);
                env = nullptr;
            }
        }
        else
        {
            env = nullptr;
        }
    }
}

void CLMDB::DefData(std::string &path, std::string &db, std::string &key, std::string &data)
{
    StrToLower(db);
    StrToLower(key);

    Event event;
    event.m_event = Event::Type::UPDATE;
    event.m_path = std::move(path);
    event.m_db = std::move(db);
    event.m_key = std::move(key);
    event.m_data = std::move(data);

    SetData(event);
}

void CLMDB::SetData(std::string &path, std::string &db, std::string &key, std::string &data)
{
    StrToLower(db);
    StrToLower(key);

    std::string evKey = db + "/"+ key;

    LockGuard lock(m_ebMtx);
    Event &event = m_eventBuffer[evKey]

    event.m_event = Event::Type::UPDATE;
    event.m_path = std::move(path);
    event.m_db = std::move(db);
    event.m_key = std::move(key);
    event.m_data = std::move(data);
}

void CLMDB::DelData(std::string path, std::string db, std::string key)
{
    StrToLower(db);
    StrToLower(key);

    std::string key = path+":"+parent + ":" + id;

    UniqueLock lock(m_ebMtx);
    m_eventBuffer.erase(key);
    lock.unlock();

    Event event;
    event.m_path = std::move(path);
    event.m_db = std::move(db);
    event.m_key = std::move(key);

    DelData(event);
}

CLMDB::Cursor CLMDB::GetCursor(std::string path, std::string db, std::string expr)
{
    Cursor retVal;

    if (expr.find("*") != std::string::npos)
    {
        MDB_env *m_env = CLMDB::GetMDB(path);

        if (m_env != nullptr)
        {
            MDB_txn_data *data = (MDB_txn_data*)corto_threadTlsGet(CLMDB_TLS_KEY);

            if (data == nullptr)
            {
                data = (MDB_txn_data*)malloc(sizeof(MDB_txn_data));
                data->txn = nullptr;
                data->refCount = 0;
                corto_threadTlsSet(CLMDB_TLS_KEY, data);
            }

            if (data->txn == nullptr)
            {
                MDB_txn *txn = nullptr;
                if (mdb_txn_begin(m_env, nullptr, MDB_RDONLY, &txn) == 0)
                {
                    MDB_dbi dbi = 0;
                    if (mdb_dbi_open(txn, db.c_str(), 0, &dbi) == 0)
                    {
                        MDB_cursor *cur;
                        if (mdb_cursor_open(txn, dbi, &cur) == 0)
                        {
                            data->txn = txn;
                            data->refCount = 1;

                            retVal.m_cursor = cur;
                            retVal.Begin();

                        }
                        else
                        {
                            mdb_txn_abort(txn);
                        }
                    }
                    else
                    {
                        mdb_txn_abort(txn);
                    }
                }
            }
            else
            {
                MDB_dbi dbi = 0;
                if (mdb_dbi_open(data->txn, db.c_str(), 0, &dbi) == 0)
                {
                    MDB_cursor *cur;
                    if (mdb_cursor_open(data->txn, dbi, &cur) == 0)
                    {
                        data->refCount++;
                        retVal.m_cursor = cur;
                        retVal.Begin();
                    }
                }
            }
        }
    }
    else
    {
        int rc = GetData(path, db, expr, retVal.m_data);

        if (rc == 0)
        {
            retVal.m_single = true;

            retVal.m_hasNext = true;
            retVal.m_name = expr;
        }
    }

    return std::move(retVal);
}