#include <string>

#include "receiver.hpp"
#include "../socket/socket.hpp"
#include "../sender_proc/sender.hpp"
#include "../socket/packet.hpp"
#include "../sql_access.hpp"
#include "../data_processor.hpp"
#include "../include/process.hpp"
#include "../include/task.hpp"
#include "../include/debug_rp.hpp"

std::string tableId = "";
std::string insertCmd, createCmd, dropCmd;
struct data_proc_container *dataProcContainer;

enum receive_stat{
    SND_EMPTY_DAT = 0,
    SND_RESET,
    P_ERR,
    P_SUCCESS,
};

int countCols(std::string data){
    int i = -1;
    int cols = 0;
    while(data[i++] != '\n'){
        if(data[i] == ',')
            cols++;
    }
    //count the last col as it doesn't end with ','
    DEBUG_MSG(__func__, "number of columns:", cols+1);
    return cols + 1;
}

int createSqlCmds(int cols, std::string body)
{
    int i, start = 0, end = 0;
    createCmd = "CREATE TABLE " + tableId + " (";
    insertCmd = "INSERT INTO "+ tableId +" (";
    std::string *colHeaders = new std::string[cols];
    
    for(i = 0; i < cols; i++)
    {
        while(end < body.length()){
            if(body[end] == ',' || body[end] == '\n')
                break;
            end++;
        }
        std::string colFeild = body.substr(start, end - start);
        createCmd.append(colFeild).append(" varchar(30) NOT NULL");
        insertCmd.append(colFeild);
        colHeaders[i] = colFeild;
        createCmd.append(",");
        start = ++end;
    
        if(i < cols - 1){
            
            insertCmd.append(",");
        }
    }

    createCmd.append("ID INTEGER PRIMARY KEY AUTOINCREMENT);");
    insertCmd.append(") VALUES (");
    dataProcContainer->colHeaders = colHeaders;
    //DEBUG_MSG(__func__, "sql createcmd:", createCmd);
    //DEBUG_MSG(__func__, "sql insertcmd:", insertCmd);

    return end;
}

int insert_into_table(std::string data, int cols, int startIndex)
{
    int rc, i, end, start, rows =0;
    std::string tempInsert;

    dataProcContainer->rows = 0;
    end = start = startIndex;
    //construct table
    const char *sqlCmd = createCmd.c_str();
    sql_write(sqlCmd);
    DEBUG_MSG(__func__,"\n");
    //insert values into table
    while(end < data.length())
    {
        tempInsert = insertCmd;
        //iterate through one col at a time
        for(i = 0; i < cols; i++)
        {
             while(end < data.length()){
                if(data[end] == ',' || data[end] == '\n')
                    break;
                end++;
            }
            //get single row value at a time
            tempInsert.append("'" + data.substr(start, end - start) + "'");
            if(i < cols - 1)
                tempInsert.append(",");
            start = ++end;
        }
        //finish insert command
        tempInsert.append(");");
        //DEBUG_MSG(__func__, "constructed insert cmd:", tempInsert);
        const char *sqlInsert = tempInsert.c_str();
        rc = sql_write(sqlInsert);
        if(rc == SQLITE_OK){
            DEBUG_MSG(__func__, "nos rows into table:",dataProcContainer->rows);
            dataProcContainer->rows++;
        }
    }

    return 0;
}

void drop_table()
{
    int rc;

    DEBUG_MSG(__func__, "dropping current table in database...");
    dropCmd = "DROP TABLE " + tableId + ";";
    sql_write(dropCmd.c_str());
}

int process_packet(struct receiver *recv)
{
    struct table *tData;
    json packet = recv->packet;
    std::string bodyData;
    int cols;
    int bodyDataStart, priority, algoType;

    //Even if one fails we flag error and not proceed further
    try{
        tableId = recv->tableID = packet["body"]["id"];
        bodyData = packet["body"]["data"];
        algoType = packet["body"]["type"];
        priority = packet["body"]["priority"];
    }catch(json::exception e){
        DEBUG_ERR(__func__, e.what());
        return P_ERR;
    }

    //initilize container
    dataProcContainer = new data_proc_container;
    recv->container = dataProcContainer;
    dataProcContainer->cols =  cols = countCols(bodyData);    
    bodyDataStart = createSqlCmds(cols, bodyData);

    //create table and insert into table;
    if(insert_into_table(bodyData, cols, bodyDataStart) == EXIT_FAILURE){
        //drop table before we fail
        drop_table();
        return EXIT_FAILURE;
    }
    tData = new table(dataProcContainer->rows, dataProcContainer->cols);
    recv->container->tData = tData;
    tData->tableID = tableId;
    tData->algorithmType = algoType;
    tData->priority = priority;

    return P_SUCCESS;
}

int identify_packet(struct receiver *recv)
{
    int rc;
    int packetHead;

    try{
        packetHead = recv->packet["head"];
    }catch(json::exception e){
        DEBUG_ERR(__func__, e.what());
        return P_ERR;
    }
    
    switch(packetHead)
    {
        case P_HANDSHAKE:
            computeID = recv->packet["id"];
            if((packetHead & (P_HANDSHAKE | P_DATSENT))){
                DEBUG_MSG(__func__, "only handshake done");
                rc = SND_EMPTY_DAT;
                break;
            }
        case P_DATSENT:
            DEBUG_MSG(__func__, "process packet");
            rc = process_packet(recv);
            break;
        case P_RESET:
        default:
            rc = SND_RESET;
    }

    return rc;
}

int receiver_proccess(void *data)
{
    struct receiver *recv = (struct receiver*)data;
    recv->receiverStatus = identify_packet(recv);
    //if(recv->receiverStatus == EXIT_FAILURE){
    //    return JOB_FAILED;
    //}
    return JOB_DONE;
}

int receiver_finalize(void *data)
{
    struct receiver *recv = (struct receiver*) data;
    
    if(recv->receiverStatus == SND_EMPTY_DAT)
    {
        DEBUG_MSG(__func__, "sending empty packet");
        send_packet("","", 100);
    } 
    else if(recv->receiverStatus == P_ERR) {
        //tableID itself is corrput or it was a status signal that was lost in transmission
        if(recv->tableID.empty())
        {
            DEBUG_MSG(__func__, "packet corrupt");
            send_packet("","", RECV_ERR);
        } else {
            //notify server to resend data
            DEBUG_ERR(__func__, "packet error encountered, resend packet");
            send_packet("", recv->tableID, PROC_ERR);
        }
        
    } else {
        //notify server data received successfully
        DEBUG_MSG(__func__,"packet received successfully");
        send_packet("", recv->tableID, NO_ERR);
        //container should be derefrenced after this as its deleted by dataprocessor
        init_data_processor(recv->thread, recv->container);
    }
    delete recv;
    
    return JOB_FINISHED;
}

struct process* receiver_proc = new process {
    .start_proc = receiver_proccess,
    .end_proc = receiver_finalize
};

int init_receiver(struct thread_pool* thread, json pkt)
{
    struct receiver *recv = new receiver;
    int rc = 0;

    DEBUG_MSG(__func__, "init receiver");
    recv->packet = pkt;
    recv->thread = thread;
    recv->packet = pkt;
    recv->packetStatus = 0;
    //this task takes higher priority than all
    sched_task(thread, receiver_proc, (void*)recv, 0);
    return 0;
}