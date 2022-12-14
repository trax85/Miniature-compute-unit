#ifndef SQL_ACC_H
#define SQL_ACC_H
#include <sqlite3.h>
#include <string>
#include <semaphore.h>

extern sem_t db_lock;

extern sqlite3* db;
sqlite3* init_db(void);
int sql_write(const char * sqlQuery);
std::string* get_row(struct table* tData, int rowNum);
int sql_read_value(const char* sqlQuery, int* value);

#endif