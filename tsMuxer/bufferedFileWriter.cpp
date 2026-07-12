
#include "bufferedFileWriter.h"

#include <fs/systemlog.h>

#include <memory>

void WriterData::execute() const
{
    switch (m_command)
    {
    case Commands::wdWrite:
    {
        // Own the buffer via RAII so it is freed even if write() throws (e.g. disk full).
        const std::unique_ptr<uint8_t[]> bufferGuard(m_buffer);
        if (m_mainFile)
        {
            m_mainFile->write(m_buffer, m_bufferLen);
        }
        break;
    }
    default:
        break;
    }
}

BufferedFileWriter::BufferedFileWriter() : m_terminated(false), m_writeQueue(WRITE_QUEUE_MAX_SIZE)
{
    m_lastErrorCode = 0;
    run(this);
}

BufferedFileWriter::~BufferedFileWriter()
{
    terminate();
    while (!m_writeQueue.empty())
    {
        WriterData writerData = m_writeQueue.pop();
        try
        {
            writerData.execute();  // RAII frees the buffer even if the write fails
        }
        catch (...)
        {
            // In teardown: never let a write failure throw out of a destructor.
        }
    }
}

void BufferedFileWriter::setError(const std::string& msg)
{
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastErrorStr = msg;
    }
    m_lastErrorCode.store(-1);  // publish only after the message is stored
}

void BufferedFileWriter::thread_main()
{
    while (!m_terminated)
    {
        WriterData writerData = m_writeQueue.peek();
        if (m_lastErrorCode.load() == 0)
        {
            try
            {
                writerData.execute();
            }
            catch (std::runtime_error& e)
            {
                setError(e.what());
                LTRACE(LT_ERROR, 0, "BufferedFileWriter::thread_main() throws runtime_error: " << e.what());
            }
            catch (std::exception& e)
            {
                setError(e.what());
                LTRACE(LT_ERROR, 0, "BufferedFileWriter::thread_main() throws exception: " << e.what());
            }
            catch (...)
            {
                setError("Unknown exception");
                LTRACE(LT_ERROR, 0, "BufferedFileWriter::thread_main() throws unknown exception");
            }
        }
        else if (writerData.m_command == WriterData::Commands::wdWrite)
        {
            // Already failed once: drain remaining buffers without retrying writes that would just fail again.
            delete[] writerData.m_buffer;
        }
        m_writeQueue.pop();
    }
}

void BufferedFileWriter::terminate()
{
    if (m_terminated)
        return;
    m_terminated = true;
    WriterData data;
    data.m_command = WriterData::Commands::wdNone;
    m_writeQueue.push(data);
    join();
}
