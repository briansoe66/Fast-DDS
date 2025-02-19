// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file EDPSimpleListener.cpp
 *
 */

#include <rtps/builtin/discovery/endpoint/EDPSimpleListeners.h>

#include <mutex>

#include <fastdds/dds/log/Log.hpp>
#include <fastdds/rtps/builtin/data/ParticipantProxyData.h>
#include <fastdds/rtps/builtin/data/ReaderProxyData.h>
#include <fastdds/rtps/builtin/data/WriterProxyData.h>
#include <fastdds/rtps/builtin/discovery/endpoint/EDPSimple.h>
#include <fastdds/rtps/builtin/discovery/participant/PDPSimple.h>
#include <fastdds/rtps/common/InstanceHandle.h>
#include <fastdds/rtps/history/ReaderHistory.h>
#include <fastdds/rtps/history/WriterHistory.h>
#include <fastdds/rtps/reader/StatefulReader.h>
#include <fastdds/rtps/writer/StatefulWriter.h>

#include <fastdds/core/policy/ParameterList.hpp>
#include <rtps/network/NetworkFactory.h>

using ParameterList = eprosima::fastdds::dds::ParameterList;

// Release reader lock to avoid ABBA lock. PDP mutex should always be first.
// Keep change information on local variables to check consistency later
#define PREVENT_PDP_DEADLOCK(reader, change, pdp)                         \
    GUID_t writer_guid = (change)->writerGUID;                            \
    SequenceNumber_t seq_num = (change)->sequenceNumber;                  \
    (reader)->getMutex().unlock();                                        \
    std::unique_lock<std::recursive_mutex> lock(*((pdp)->getMutex()));    \
    (reader)->getMutex().lock();                                          \
                                                                          \
    if ((ALIVE != (change)->kind) ||                                      \
            (seq_num != (change)->sequenceNumber) ||                      \
            (writer_guid != (change)->writerGUID))                        \
    {                                                                     \
        return;                                                           \
    }                                                                     \
    (void)seq_num

namespace eprosima {
namespace fastrtps {
namespace rtps {

void EDPBasePUBListener::add_writer_from_change(
        RTPSReader* reader,
        ReaderHistory* reader_history,
        CacheChange_t* change,
        EDP* edp,
        bool release_change /*=true*/)
{
    //LOAD INFORMATION IN DESTINATION WRITER PROXY DATA
    const NetworkFactory& network = edp->mp_RTPSParticipant->network_factory();
    CDRMessage_t tempMsg(change->serializedPayload);
    auto temp_writer_data = edp->get_temporary_writer_proxies_pool().get();

    if (temp_writer_data->readFromCDRMessage(&tempMsg, network,
            edp->mp_RTPSParticipant->has_shm_transport()))
    {
        if (temp_writer_data->guid().guidPrefix == edp->mp_RTPSParticipant->getGuid().guidPrefix)
        {
            EPROSIMA_LOG_INFO(RTPS_EDP, "Message from own RTPSParticipant, ignoring");
            return;
        }

        //LOAD INFORMATION IN DESTINATION WRITER PROXY DATA
        auto copy_data_fun = [&temp_writer_data, &network](
            WriterProxyData* data,
            bool updating,
            const ParticipantProxyData& participant_data)
                {
                    if (!temp_writer_data->has_locators())
                    {
                        temp_writer_data->set_remote_locators(participant_data.default_locators, network, true);
                    }

                    if (updating && !data->is_update_allowed(*temp_writer_data))
                    {
                        EPROSIMA_LOG_WARNING(RTPS_EDP,
                                "Received incompatible update for WriterQos. writer_guid = " << data->guid());
                    }
                    *data = *temp_writer_data;
                    return true;
                };

        GUID_t participant_guid;
        WriterProxyData* writer_data =
                edp->mp_PDP->addWriterProxyData(temp_writer_data->guid(), participant_guid, copy_data_fun);

        // release temporary proxy
        temp_writer_data.reset();

        //Removing change from history
        reader_history->remove_change(reader_history->find_change(change), release_change);

        // At this point we can release reader lock, cause change is not used
        reader->getMutex().unlock();
        if (writer_data != nullptr)
        {
            edp->pairing_writer_proxy_with_any_local_reader(participant_guid, writer_data);
        }
        else //NOT ADDED BECAUSE IT WAS ALREADY THERE
        {
            EPROSIMA_LOG_WARNING(RTPS_EDP, "Received message from UNKNOWN RTPSParticipant, removing");
        }
        // Take again the reader lock.
        reader->getMutex().lock();
    }
}

void EDPSimplePUBListener::onNewCacheChangeAdded(
        RTPSReader* reader,
        const CacheChange_t* const change_in)
{
    CacheChange_t* change = (CacheChange_t*)change_in;
    //std::lock_guard<std::recursive_mutex> guard(*this->sedp_->publications_reader_.first->getMutex());
    EPROSIMA_LOG_INFO(RTPS_EDP, "");
    if (!computeKey(change))
    {
        EPROSIMA_LOG_WARNING(RTPS_EDP, "Received change with no Key");
    }

    ReaderHistory* reader_history =
#if HAVE_SECURITY
            reader == sedp_->publications_secure_reader_.first ?
            sedp_->publications_secure_reader_.second :
#endif // if HAVE_SECURITY
            sedp_->publications_reader_.second;

    if (change->kind == ALIVE)
    {
        PREVENT_PDP_DEADLOCK(reader, change, sedp_->mp_PDP);

        // Note: change is removed from history inside this method.
        add_writer_from_change(reader, reader_history, change, sedp_);
    }
    else
    {
        //REMOVE WRITER FROM OUR READERS:
        EPROSIMA_LOG_INFO(RTPS_EDP, "Disposed Remote Writer, removing...");
        GUID_t writer_guid = iHandle2GUID(change->instanceHandle);
        //Removing change from history
        reader_history->remove_change(change);
        reader->getMutex().unlock();
        this->sedp_->mp_PDP->removeWriterProxyData(writer_guid);
        reader->getMutex().lock();
    }
}

bool EDPListener::computeKey(
        CacheChange_t* change)
{
    return ParameterList::readInstanceHandleFromCDRMsg(change, fastdds::dds::PID_ENDPOINT_GUID);
}

void EDPBaseSUBListener::add_reader_from_change(
        RTPSReader* reader,
        ReaderHistory* reader_history,
        CacheChange_t* change,
        EDP* edp,
        bool release_change /*=true*/)
{
    //LOAD INFORMATION IN TEMPORAL WRITER PROXY DATA
    const NetworkFactory& network = edp->mp_RTPSParticipant->network_factory();
    CDRMessage_t tempMsg(change->serializedPayload);
    auto temp_reader_data = edp->get_temporary_reader_proxies_pool().get();

    if (temp_reader_data->readFromCDRMessage(&tempMsg, network,
            edp->mp_RTPSParticipant->has_shm_transport()))
    {
        if (temp_reader_data->guid().guidPrefix == edp->mp_RTPSParticipant->getGuid().guidPrefix)
        {
            EPROSIMA_LOG_INFO(RTPS_EDP, "From own RTPSParticipant, ignoring");
            return;
        }

        auto copy_data_fun = [&temp_reader_data, &network](
            ReaderProxyData* data,
            bool updating,
            const ParticipantProxyData& participant_data)
                {
                    if (!temp_reader_data->has_locators())
                    {
                        temp_reader_data->set_remote_locators(participant_data.default_locators, network, true);
                    }

                    if (updating && !data->is_update_allowed(*temp_reader_data))
                    {
                        EPROSIMA_LOG_WARNING(RTPS_EDP,
                                "Received incompatible update for ReaderQos. reader_guid = " << data->guid());
                    }
                    *data = *temp_reader_data;
                    return true;
                };

        //LOOK IF IS AN UPDATED INFORMATION
        GUID_t participant_guid;
        ReaderProxyData* reader_data =
                edp->mp_PDP->addReaderProxyData(temp_reader_data->guid(), participant_guid, copy_data_fun);

        // Release the temporary proxy
        temp_reader_data.reset();

        // Remove change from history.
        reader_history->remove_change(reader_history->find_change(change), release_change);

        // At this point we can release reader lock, cause change is not used
        reader->getMutex().unlock();

        if (reader_data != nullptr) //ADDED NEW DATA
        {
            edp->pairing_reader_proxy_with_any_local_writer(participant_guid, reader_data);

        }
        else
        {
            EPROSIMA_LOG_WARNING(RTPS_EDP, "From UNKNOWN RTPSParticipant, removing");
        }

        // Take again the reader lock.
        reader->getMutex().lock();
    }
}

void EDPSimpleSUBListener::onNewCacheChangeAdded(
        RTPSReader* reader,
        const CacheChange_t* const change_in)
{
    CacheChange_t* change = (CacheChange_t*)change_in;
    //std::lock_guard<std::recursive_mutex> guard(*this->sedp_->subscriptions_reader_.first->getMutex());
    EPROSIMA_LOG_INFO(RTPS_EDP, "");
    if (!computeKey(change))
    {
        EPROSIMA_LOG_WARNING(RTPS_EDP, "Received change with no Key");
    }

    ReaderHistory* reader_history =
#if HAVE_SECURITY
            reader == sedp_->subscriptions_secure_reader_.first ?
            sedp_->subscriptions_secure_reader_.second :
#endif // if HAVE_SECURITY
            sedp_->subscriptions_reader_.second;

    if (change->kind == ALIVE)
    {
        PREVENT_PDP_DEADLOCK(reader, change, sedp_->mp_PDP);

        // Note: change is removed from history inside this method.
        add_reader_from_change(reader, reader_history, change, sedp_);
    }
    else
    {
        //REMOVE WRITER FROM OUR READERS:
        EPROSIMA_LOG_INFO(RTPS_EDP, "Disposed Remote Reader, removing...");

        GUID_t reader_guid = iHandle2GUID(change->instanceHandle);
        //Removing change from history
        reader_history->remove_change(change);
        reader->getMutex().unlock();
        this->sedp_->mp_PDP->removeReaderProxyData(reader_guid);
        reader->getMutex().lock();
    }
}

void EDPSimplePUBListener::onWriterChangeReceivedByAll(
        RTPSWriter* writer,
        CacheChange_t* change)
{
    (void)writer;

    if (ChangeKind_t::NOT_ALIVE_DISPOSED_UNREGISTERED == change->kind)
    {
        WriterHistory* writer_history =
#if HAVE_SECURITY
                writer == sedp_->publications_secure_writer_.first ?
                sedp_->publications_secure_writer_.second :
#endif // if HAVE_SECURITY
                sedp_->publications_writer_.second;

        writer_history->remove_change(change);
    }
}

void EDPSimpleSUBListener::onWriterChangeReceivedByAll(
        RTPSWriter* writer,
        CacheChange_t* change)
{
    (void)writer;

    if (ChangeKind_t::NOT_ALIVE_DISPOSED_UNREGISTERED == change->kind)
    {
        WriterHistory* writer_history =
#if HAVE_SECURITY
                writer == sedp_->subscriptions_secure_writer_.first ?
                sedp_->subscriptions_secure_writer_.second :
#endif // if HAVE_SECURITY
                sedp_->subscriptions_writer_.second;

        writer_history->remove_change(change);
    }

}

} /* namespace rtps */
} /* namespace fastrtps */
} /* namespace eprosima */
