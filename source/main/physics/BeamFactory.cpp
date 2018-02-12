/*
    This source file is part of Rigs of Rods
    Copyright 2005-2012 Pierre-Michel Ricordel
    Copyright 2007-2012 Thomas Fischer
    Copyright 2013-2017 Petr Ohlidal & contributors

    For more information, see http://www.rigsofrods.org/

    Rigs of Rods is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 3, as
    published by the Free Software Foundation.

    Rigs of Rods is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Rigs of Rods. If not, see <http://www.gnu.org/licenses/>.
*/

/// @file
/// @author Thomas Fischer (thomas{AT}thomasfischer{DOT}biz)
/// @date   24th of August 2009

#include "BeamFactory.h"

#include "Application.h"
#include "BeamEngine.h"
#include "BeamStats.h"
#include "CacheSystem.h"
#include "ChatSystem.h"
#include "Collisions.h"
#include "DynamicCollisions.h"
#include "GUIManager.h"
#include "GUI_TopMenubar.h"
#include "Language.h"
#include "MainMenu.h"

#include "Network.h"
#include "PointColDetector.h"
#include "RigLoadingProfiler.h"
#include "RigLoadingProfilerControl.h"
#include "RoRFrameListener.h"
#include "Settings.h"
#include "SoundScriptManager.h"
#include "ThreadPool.h"
#include "Utils.h"
#include "VehicleAI.h"

#ifdef _GNU_SOURCE
#include <sys/sysinfo.h>
#endif

#if defined(__APPLE__) || defined (__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif // __APPLE__ || __FREEBSD__

#include "DashBoardManager.h"

#include <algorithm>
#include <cstring>

#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
  #include <intrin.h>
#endif

using namespace Ogre;

void cpuID(unsigned i, unsigned regs[4])
{
#ifdef _WIN32
    __cpuid((int *)regs, (int)i);
#elif defined(__x86_64__) || defined(__i386)
    asm volatile
        ("cpuid" : "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3])
         : "a" (i), "c" (0));
#endif
}

static unsigned hardware_concurrency()
{
#if defined(_GNU_SOURCE)
        return get_nprocs();
#elif defined(__APPLE__) || defined(__FreeBSD__)
        int count;
        size_t size = sizeof(count);
        return sysctlbyname("hw.ncpu", &count, &size, NULL, 0) ? 0 : count;
#elif defined(BOOST_HAS_UNISTD_H) && defined(_SC_NPROCESSORS_ONLN)
        int const count = sysconf(_SC_NPROCESSORS_ONLN);
        return (count > 0) ? count : 0;
#else
    return std::thread::hardware_concurrency();
#endif
}

unsigned int getNumberOfCPUCores()
{
#if defined(_WIN32) || defined(__x86_64__) || defined(__i386)
    unsigned regs[4];

    // Get CPU vendor
    char vendor[12];
    cpuID(0, regs);
    memcpy((void *)vendor,     (void *)&regs[1], 4); // EBX
    memcpy((void *)&vendor[4], (void *)&regs[3], 4); // EDX
    memcpy((void *)&vendor[8], (void *)&regs[2], 4); // ECX
    std::string cpuVendor = std::string(vendor, 12);

    // Get CPU features
    cpuID(1, regs);
    unsigned cpuFeatures = regs[3]; // EDX

    // Logical core count per CPU
    cpuID(1, regs);
    unsigned logical = (regs[1] >> 16) & 0xff; // EBX[23:16]
    unsigned cores = logical;

    if (cpuVendor == "GenuineIntel")
    {
        // Get DCP cache info
        cpuID(4, regs);
        cores = ((regs[0] >> 26) & 0x3f) + 1; // EAX[31:26] + 1
    }
    else if (cpuVendor == "AuthenticAMD")
    {
        // Get NC: Number of CPU cores - 1
        cpuID(0x80000008, regs);
        cores = ((unsigned)(regs[2] & 0xff)) + 1; // ECX[7:0] + 1
    }

    // Detect hyper-threads
    bool hyperThreads = cpuFeatures & (1 << 28) && cores < logical;

    LOG("BEAMFACTORY: " + TOSTRING(logical) + " logical CPU cores" + " found");
    LOG("BEAMFACTORY: " + TOSTRING(cores) + " CPU cores" + " found");
    LOG("BEAMFACTORY: Hyper-Threading " + TOSTRING(hyperThreads));
#else
    unsigned cores = hardware_concurrency();
    LOG("BEAMFACTORY: " + TOSTRING(cores) + " CPU cores" + " found");
#endif
    return cores;
}

using namespace RoR;

ActorManager::ActorManager(RoRFrameListener* sim_controller)
    : m_player_actor(-1)
    , m_dt_remainder(0.0f)
    , m_forced_active(false)
    , m_free_actor_slot(0)
    , m_num_cpu_cores(0)
    , m_physics_frames(0)
    , m_physics_steps(2000)
    , m_prev_player_actor(-1)
    , m_simulated_actor(0)
    , m_simulation_speed(1.0f)
    , m_sim_controller(sim_controller)
    , m_actors() // Array
{

    if (RoR::App::app_multithread.GetActive())
    {
        // Create thread pool
        int numThreadsInPool = ISETTING("NumThreadsInThreadPool", 0);

        if (numThreadsInPool > 1)
        {
            m_num_cpu_cores = numThreadsInPool;
        }
        else
        {
            int logical_cpus = hardware_concurrency();
            int physical_cpus = getNumberOfCPUCores();

            if (physical_cpus < 6 && logical_cpus > physical_cpus)
                m_num_cpu_cores = logical_cpus - 1;
            else
                m_num_cpu_cores = physical_cpus - 1;
        }

        bool disableThreadPool = BSETTING("DisableThreadPool", false);

        if (m_num_cpu_cores < 2)
        {
            disableThreadPool = true;
            LOG("BEAMFACTORY: Not enough CPU cores to enable the thread pool");
        }
        else if (!disableThreadPool)
        {
            gEnv->threadPool = new ThreadPool(m_num_cpu_cores);
            LOG("BEAMFACTORY: Creating " + TOSTRING(m_num_cpu_cores) + " threads");
        }

        // Create worker thread (used for physics calculations)
        m_sim_thread_pool = std::unique_ptr<ThreadPool>(new ThreadPool(1));
    }
}

ActorManager::~ActorManager()
{
    this->SyncWithSimThread(); // Wait for sim task to finish
    delete gEnv->threadPool;
    m_particle_manager.DustManDiscard(gEnv->sceneManager); // TODO: de-globalize SceneManager
}

#define LOADRIG_PROFILER_CHECKPOINT(ENTRY_ID) rig_loading_profiler.Checkpoint(RoR::RigLoadingProfiler::ENTRY_ID);

Actor* ActorManager::CreateLocalRigInstance(
    Ogre::Vector3 pos,
    Ogre::Quaternion rot,
    Ogre::String fname,
    int cache_entry_number, // = -1,
    collision_box_t* spawnbox /* = nullptr */,
    bool ismachine /* = false */,
    const std::vector<Ogre::String>* truckconfig /* = nullptr */,
    RoR::SkinDef* skin /* = nullptr */,
    bool freePosition, /* = false */
    bool preloaded_with_terrain /* = false */
)
{
    RoR::RigLoadingProfiler rig_loading_profiler;
#ifdef ROR_PROFILE_RIG_LOADING
    ::Profiler::reset();
#endif

    int truck_num = this->GetFreeTruckSlot();
    if (truck_num == -1)
    {
        LOG("ERROR: Could not add beam to main list");
        return 0;
    }

    Actor* b = new Actor(
        m_sim_controller,
        truck_num,
        pos,
        rot,
        fname.c_str(),
        &rig_loading_profiler,
        false, // networked
        (RoR::App::mp_state.GetActive() == RoR::MpState::CONNECTED), // networking
        spawnbox,
        ismachine,
        truckconfig,
        skin,
        freePosition,
        preloaded_with_terrain,
        cache_entry_number
    );

    if (b->ar_sim_state == Actor::SimState::INVALID)
    {
        this->DeleteTruck(b);
        return nullptr;
    }

    m_actors[truck_num] = b;

    // lock slide nodes after spawning the truck?
    if (b->getSlideNodesLockInstant())
    {
        b->toggleSlideNodeLock();
    }

    RoR::App::GetGuiManager()->GetTopMenubar()->triggerUpdateVehicleList();

    // add own username to truck
    if (RoR::App::mp_state.GetActive() == RoR::MpState::CONNECTED)
    {
        b->updateNetworkInfo();
    }
    LOADRIG_PROFILER_CHECKPOINT(ENTRY_BEAMFACTORY_CREATELOCAL_POSTPROCESS);

    LOG(rig_loading_profiler.Report());

#ifdef ROR_PROFILE_RIG_LOADING
    std::string out_path = std::string(App::sys_user_dir.GetActive()) + PATH_SLASH + "profiler" + PATH_SLASH + ROR_PROFILE_RIG_LOADING_OUTFILE;
    ::Profiler::DumpHtml(out_path.c_str());
#endif
    return b;
}

#undef LOADRIG_PROFILER_CHECKPOINT

int ActorManager::CreateRemoteInstance(RoRnet::TruckStreamRegister* reg)
{
    LOG(" new beam truck for " + TOSTRING(reg->origin_sourceid) + ":" + TOSTRING(reg->origin_streamid));

#ifdef USE_SOCKETW
    RoRnet::UserInfo info;
    RoR::Networking::GetUserInfo(reg->origin_sourceid, info);

    UTFString message = RoR::ChatSystem::GetColouredName(info.username, info.colournum) + RoR::Color::CommandColour + _L(" spawned a new vehicle: ") + RoR::Color::NormalColour + reg->name;
    RoR::App::GetGuiManager()->pushMessageChatBox(message);
#endif // USE_SOCKETW

    // check if we got this truck installed
    String filename = String(reg->name);
    String group = "";
    if (!RoR::App::GetCacheSystem()->checkResourceLoaded(filename, group))
    {
        LOG("wont add remote stream (truck not existing): '"+filename+"'");
        return -1;
    }

    // fill truckconfig
    std::vector<String> truckconfig;
    for (int t = 0; t < 10; t++)
    {
        if (!strnlen(reg->truckconfig[t], 60))
            break;
        truckconfig.push_back(String(reg->truckconfig[t]));
    }

    // DO NOT spawn the truck far off anywhere
    // the truck parsing will break flexbodies initialization when using huge numbers here
    Vector3 pos = Vector3::ZERO;

    int truck_num = this->GetFreeTruckSlot();
    if (truck_num == -1)
    {
        LOG("ERROR: could not add beam to main list");
        return -1;
    }
    RoR::RigLoadingProfiler p; // TODO: Placeholder. Use it
    Actor* b = new Actor(
        m_sim_controller,
        truck_num,
        pos,
        Quaternion::ZERO,
        reg->name,
        &p,
        true, // networked
        (RoR::App::mp_state.GetActive() == RoR::MpState::CONNECTED), // networking
        nullptr, // spawnbox
        false, // ismachine
        &truckconfig,
        nullptr // skin
    );

    if (b->ar_sim_state == Actor::SimState::INVALID)
    {
        this->DeleteTruck(b);
        return -1;
    }
    m_actors[truck_num] = b;

    b->ar_net_source_id = reg->origin_sourceid;
    b->ar_net_stream_id = reg->origin_streamid;
    b->updateNetworkInfo();


    RoR::App::GetGuiManager()->GetTopMenubar()->triggerUpdateVehicleList();


    return 1;
}

void ActorManager::RemoveStreamSource(int sourceid)
{
    m_stream_mismatches.erase(sourceid);

    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (!m_actors[t])
            continue;
        if (m_actors[t]->ar_sim_state != Actor::SimState::NETWORKED_OK)
            continue;

        if (m_actors[t]->ar_net_source_id == sourceid)
        {
            this->DeleteTruck(m_actors[t]);
        }
    }
}

#ifdef USE_SOCKETW
void ActorManager::handleStreamData(std::vector<RoR::Networking::recv_packet_t> packet_buffer)
{
    for (auto packet : packet_buffer)
    {
        if (packet.header.command == RoRnet::MSG2_STREAM_REGISTER)
        {
            RoRnet::StreamRegister* reg = (RoRnet::StreamRegister *)packet.buffer;
            if (reg->type == 0)
            {
                reg->status = this->CreateRemoteInstance((RoRnet::TruckStreamRegister *)packet.buffer);
                RoR::Networking::AddPacket(0, RoRnet::MSG2_STREAM_REGISTER_RESULT, sizeof(RoRnet::StreamRegister), (char *)reg);
            }
        }
        else if (packet.header.command == RoRnet::MSG2_STREAM_REGISTER_RESULT)
        {
            RoRnet::StreamRegister* reg = (RoRnet::StreamRegister *)packet.buffer;
            for (int t = 0; t < m_free_actor_slot; t++)
            {
                if (!m_actors[t])
                    continue;
                if (m_actors[t]->ar_sim_state == Actor::SimState::NETWORKED_OK)
                    continue;
                if (m_actors[t]->ar_net_stream_id == reg->origin_streamid)
                {
                    int sourceid = packet.header.source;
                    m_actors[t]->ar_net_stream_results[sourceid] = reg->status;

                    if (reg->status == 1)
                    LOG("Client " + TOSTRING(sourceid) + " successfully loaded stream " + TOSTRING(reg->origin_streamid) + " with name '" + reg->name + "', result code: " + TOSTRING(reg->status));
                    else
                    LOG("Client " + TOSTRING(sourceid) + " could not load stream " + TOSTRING(reg->origin_streamid) + " with name '" + reg->name + "', result code: " + TOSTRING(reg->status));

                    break;
                }
            }
        }
        else if (packet.header.command == RoRnet::MSG2_STREAM_UNREGISTER)
        {
            Actor* b = this->getBeam(packet.header.source, packet.header.streamid);
            if (b && b->ar_sim_state == Actor::SimState::NETWORKED_OK)
            {
                this->DeleteTruck(b);
            }
            auto search = m_stream_mismatches.find(packet.header.source);
            if (search != m_stream_mismatches.end())
            {
                auto& mismatches = search->second;
                auto it = std::find(mismatches.begin(), mismatches.end(), packet.header.streamid);
                if (it != mismatches.end())
                    mismatches.erase(it);
            }
        }
        else if (packet.header.command == RoRnet::MSG2_USER_LEAVE)
        {
            this->RemoveStreamSource(packet.header.source);
        }
        else
        {
            for (int t = 0; t < m_free_actor_slot; t++)
            {
                if (!m_actors[t])
                    continue;
                if (m_actors[t]->ar_sim_state != Actor::SimState::NETWORKED_OK)
                    continue;

                m_actors[t]->receiveStreamData(packet.header.command, packet.header.source, packet.header.streamid, packet.buffer, packet.header.size);
            }
        }
    }
}
#endif // USE_SOCKETW

int ActorManager::checkStreamsOK(int sourceid)
{
    if (m_stream_mismatches[sourceid].size() > 0)
        return 0;

    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (!m_actors[t])
            continue;
        if (m_actors[t]->ar_sim_state != Actor::SimState::NETWORKED_OK)
            continue;

        if (m_actors[t]->ar_net_source_id == sourceid)
        {
            return 1;
        }
    }

    return 2;
}

int ActorManager::checkStreamsRemoteOK(int sourceid)
{
    int result = 2;

    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (!m_actors[t])
            continue;
        if (m_actors[t]->ar_sim_state == Actor::SimState::NETWORKED_OK)
            continue;

        int stream_result = m_actors[t]->ar_net_stream_results[sourceid];
        if (stream_result == -1)
            return 0;
        if (stream_result == 1)
            result = 1;
    }

    return result;
}

Actor* ActorManager::getBeam(int source_id, int stream_id)
{
    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (!m_actors[t])
            continue;
        if (m_actors[t]->ar_sim_state != Actor::SimState::NETWORKED_OK)
            continue;

        if (m_actors[t]->ar_net_source_id == source_id && m_actors[t]->ar_net_stream_id == stream_id)
        {
            return m_actors[t];
        }
    }

    return nullptr;
}

bool ActorManager::intersectionAABB(Ogre::AxisAlignedBox a, Ogre::AxisAlignedBox b, float scale)
{
    if (scale != 1.0f)
    {
        Vector3 a_center = a.getCenter();
        Vector3 a_half_size = a.getHalfSize();
        a.setMaximum(a_center + a_half_size * scale);
        a.setMinimum(a_center - a_half_size * scale);

        Vector3 b_center = b.getCenter();
        Vector3 b_half_size = b.getHalfSize();
        b.setMaximum(b_center + b_half_size * scale);
        b.setMinimum(b_center - b_half_size * scale);
    }

    return a.intersects(b);
}

bool ActorManager::truckIntersectionAABB(int a, int b, float scale)
{
    return intersectionAABB(m_actors[a]->ar_bounding_box, m_actors[b]->ar_bounding_box, scale);
}

bool ActorManager::predictTruckIntersectionAABB(int a, int b, float scale)
{
    return intersectionAABB(m_actors[a]->ar_predicted_bounding_box, m_actors[b]->ar_predicted_bounding_box, scale);
}

bool ActorManager::truckIntersectionCollAABB(int a, int b, float scale)
{
    if (m_actors[a]->ar_collision_bounding_boxes.empty() && m_actors[b]->ar_collision_bounding_boxes.empty())
    {
        return truckIntersectionAABB(a, b, scale);
    }
    else if (m_actors[a]->ar_collision_bounding_boxes.empty())
    {
        for (std::vector<AxisAlignedBox>::iterator it = m_actors[b]->ar_collision_bounding_boxes.begin(); it != m_actors[b]->ar_collision_bounding_boxes.end(); ++it)
            if (intersectionAABB(*it, m_actors[a]->ar_bounding_box, scale))
                return true;
    }
    else if (m_actors[b]->ar_collision_bounding_boxes.empty())
    {
        for (std::vector<AxisAlignedBox>::iterator it = m_actors[a]->ar_collision_bounding_boxes.begin(); it != m_actors[a]->ar_collision_bounding_boxes.end(); ++it)
            if (intersectionAABB(*it, m_actors[b]->ar_bounding_box, scale))
                return true;
    }
    else
    {
        for (std::vector<AxisAlignedBox>::iterator it_a = m_actors[a]->ar_collision_bounding_boxes.begin(); it_a != m_actors[a]->ar_collision_bounding_boxes.end(); ++it_a)
            for (std::vector<AxisAlignedBox>::iterator it_b = m_actors[b]->ar_collision_bounding_boxes.begin(); it_b != m_actors[b]->ar_collision_bounding_boxes.end(); ++it_b)
                if (intersectionAABB(*it_a, *it_b, scale))
                    return true;
    }

    return false;
}

bool ActorManager::predictTruckIntersectionCollAABB(int a, int b, float scale)
{
    if (m_actors[a]->ar_predicted_coll_bounding_boxes.empty() && m_actors[b]->ar_predicted_coll_bounding_boxes.empty())
    {
        return predictTruckIntersectionAABB(a, b, scale);
    }
    else if (m_actors[a]->ar_predicted_coll_bounding_boxes.empty())
    {
        for (std::vector<AxisAlignedBox>::iterator it = m_actors[b]->ar_predicted_coll_bounding_boxes.begin(); it != m_actors[b]->ar_predicted_coll_bounding_boxes.end(); ++it)
            if (intersectionAABB(*it, m_actors[a]->ar_predicted_bounding_box, scale))
                return true;
    }
    else if (m_actors[b]->ar_predicted_coll_bounding_boxes.empty())
    {
        for (std::vector<AxisAlignedBox>::iterator it = m_actors[a]->ar_predicted_coll_bounding_boxes.begin(); it != m_actors[a]->ar_predicted_coll_bounding_boxes.end(); ++it)
            if (intersectionAABB(*it, m_actors[b]->ar_predicted_bounding_box, scale))
                return true;
    }
    else
    {
        for (std::vector<AxisAlignedBox>::iterator it_a = m_actors[a]->ar_predicted_coll_bounding_boxes.begin(); it_a != m_actors[a]->ar_predicted_coll_bounding_boxes.end(); ++it_a)
            for (std::vector<AxisAlignedBox>::iterator it_b = m_actors[b]->ar_predicted_coll_bounding_boxes.begin(); it_b != m_actors[b]->ar_predicted_coll_bounding_boxes.end(); ++it_b)
                if (intersectionAABB(*it_a, *it_b, scale))
                    return true;
    }

    return false;
}

void ActorManager::RecursiveActivation(int j, std::bitset<MAX_TRUCKS>& visited)
{
    if (visited[j] || !m_actors[j] || m_actors[j]->ar_sim_state != Actor::SimState::LOCAL_SIMULATED)
        return;

    visited.set(j, true);

    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (t == j || !m_actors[t] || visited[t])
            continue;
        if (m_actors[t]->ar_sim_state == Actor::SimState::LOCAL_SIMULATED && truckIntersectionCollAABB(t, j, 1.2f))
        {
            m_actors[t]->ar_sleep_counter = 0.0f;
            this->RecursiveActivation(t, visited);
        }
        if (m_actors[t]->ar_sim_state == Actor::SimState::LOCAL_SLEEPING && predictTruckIntersectionCollAABB(t, j))
        {
            m_actors[t]->ar_sleep_counter = 0.0f;
            m_actors[t]->ar_sim_state = Actor::SimState::LOCAL_SIMULATED;
            this->RecursiveActivation(t, visited);
        }
    }
}

void ActorManager::UpdateSleepingState(float dt)
{
    if (!m_forced_active)
    {
        for (int t = 0; t < m_free_actor_slot; t++)
        {
            if (!m_actors[t])
                continue;
            if (m_actors[t]->ar_sim_state != Actor::SimState::LOCAL_SIMULATED)
                continue;
            if (m_actors[t]->getVelocity().squaredLength() > 0.01f)
                continue;

            m_actors[t]->ar_sleep_counter += dt;

            if (m_actors[t]->ar_sleep_counter >= 10.0f)
            {
                m_actors[t]->ar_sim_state = Actor::SimState::LOCAL_SLEEPING;
            }
        }
    }

    Actor* current_truck = this->GetPlayerActorInternal();
    if (current_truck && current_truck->ar_sim_state == Actor::SimState::LOCAL_SLEEPING)
    {
        current_truck->ar_sim_state = Actor::SimState::LOCAL_SIMULATED;
    }

    std::bitset<MAX_TRUCKS> visited;
    // Recursivly activate all trucks which can be reached from current_truck
    if (current_truck && current_truck->ar_sim_state == Actor::SimState::LOCAL_SIMULATED)
    {
        current_truck->ar_sleep_counter = 0.0f;
        this->RecursiveActivation(m_player_actor, visited);
    }
    // Snowball effect (activate all trucks which might soon get hit by a moving truck)
    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (m_actors[t] && m_actors[t]->ar_sim_state == Actor::SimState::LOCAL_SIMULATED && m_actors[t]->ar_sleep_counter == 0.0f)
            this->RecursiveActivation(t, visited);
    }
}

int ActorManager::GetFreeTruckSlot()
{
    // find a free slot for the truck
    for (int t = 0; t < MAX_TRUCKS; t++)
    {
        if (!m_actors[t] && t >= m_free_actor_slot) // XXX: TODO: remove this hack
        {
            // reuse slots
            if (t >= m_free_actor_slot)
                m_free_actor_slot = t + 1;
            return t;
        }
    }
    return -1;
}

void ActorManager::activateAllTrucks()
{
    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (m_actors[t] && m_actors[t]->ar_sim_state == Actor::SimState::LOCAL_SLEEPING)
        {
            m_actors[t]->ar_sim_state = Actor::SimState::LOCAL_SIMULATED;
            m_actors[t]->ar_sleep_counter = 0.0f;

            if (this->getTruck(m_simulated_actor))
            {
                m_actors[t]->ar_disable_aerodyn_turbulent_drag = this->getTruck(m_simulated_actor)->ar_driveable == AIRPLANE;
            }
        }
    }
}

void ActorManager::sendAllTrucksSleeping()
{
    m_forced_active = false;
    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (m_actors[t] && m_actors[t]->ar_sim_state == Actor::SimState::LOCAL_SIMULATED)
        {
            m_actors[t]->ar_sim_state = Actor::SimState::LOCAL_SLEEPING;
        }
    }
}

void ActorManager::recalcGravityMasses()
{
    // update the mass of all trucks
    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (m_actors[t])
        {
            m_actors[t]->recalc_masses();
        }
    }
}

int ActorManager::FindTruckInsideBox(Collisions* collisions, const Ogre::String& inst, const Ogre::String& box)
{
    // try to find the desired truck (the one in the box)
    int id = -1;
    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (!m_actors[t])
            continue;
        if (collisions->isInside(m_actors[t]->ar_nodes[0].AbsPosition, inst, box))
        {
            if (id == -1)
            // first truck found
                id = t;
            else
            // second truck found -> unclear which vehicle was meant
                return -1;
        }
    }
    return id;
}

void ActorManager::repairTruck(Collisions* collisions, const Ogre::String& inst, const Ogre::String& box, bool keepPosition)
{
    int rtruck = this->FindTruckInsideBox(collisions, inst, box);
    if (rtruck >= 0)
    {
        // take a position reference
        SOUND_PLAY_ONCE(rtruck, SS_TRIG_REPAIR);

        Vector3 ipos = m_actors[rtruck]->ar_nodes[0].AbsPosition;
        m_actors[rtruck]->reset();
        m_actors[rtruck]->resetPosition(ipos.x, ipos.z, false, 0);
        m_actors[rtruck]->updateVisual();
    }
}

void ActorManager::MuteAllTrucks()
{
    for (int i = 0; i < m_free_actor_slot; i++)
    {
        if (m_actors[i])
        {
            m_actors[i]->StopAllSounds();
        }
    }
}

void ActorManager::UnmuteAllTrucks()
{
    for (int i = 0; i < m_free_actor_slot; i++)
    {
        if (m_actors[i])
        {
            m_actors[i]->UnmuteAllSounds();
        }
    }
}

void ActorManager::RemoveActorByCollisionBox(Collisions* collisions, const Ogre::String& inst, const Ogre::String& box)
{
    removeTruck(this->FindTruckInsideBox(collisions, inst, box));
}

void ActorManager::removeTruck(int truck)
{
    if (truck < 0 || truck > m_free_actor_slot)
        return;

    if (!m_actors[truck])
        return;

    if (m_actors[truck]->ar_sim_state == Actor::SimState::NETWORKED_OK)
        return;

    this->DeleteTruck(m_actors[truck]);
}

void ActorManager::CleanUpAllTrucks() // Called after simulation finishes
{
    for (int i = 0; i < m_free_actor_slot; i++)
    {
        if (m_actors[i] == nullptr)
            continue; // This is how things currently work (but not for long...) ~ only_a_ptr, 01/2017

        delete m_actors[i];
        m_actors[i] = nullptr;
    }

    // Reset to empty value. Do NOT call `setCurrentTruck(-1)` - performs updates which are invalid at this point
    m_player_actor = -1;

    // TEMPORARY: DO !NOT! attempt to reuse slots
    // Yields bad behavior when player disconnects from game where other players had vehicles spawned
    // Upon reconnect, vehicles with flexbodies (tested on Gavril MZR) show up badly deformed and twitching.
    // Vehicles with cabs (tested on Agora L) show up without the cab.
    // ~only_a_ptr, 01/2017
    //m_free_actor_slot = 0;
}

void ActorManager::DeleteTruck(Actor* b)
{
    if (b == 0)
        return;

    this->SyncWithSimThread();

#ifdef USE_SOCKETW
    if (b->ar_uses_networking && b->ar_sim_state != Actor::SimState::NETWORKED_OK && b->ar_sim_state != Actor::SimState::INVALID)
    {
        RoR::Networking::AddPacket(b->ar_net_stream_id, RoRnet::MSG2_STREAM_UNREGISTER, 0, 0);
    }
#endif // USE_SOCKETW

    if (m_player_actor == b->ar_instance_id)
        setCurrentTruck(-1);

    m_actors[b->ar_instance_id] = 0;
    delete b;


    RoR::App::GetGuiManager()->GetTopMenubar()->triggerUpdateVehicleList();

}

int ActorManager::GetMostRecentTruckSlot()
{
    if (getTruck(m_player_actor))
    {
        return m_player_actor;
    }
    else if (getTruck(m_prev_player_actor))
    {
        return m_prev_player_actor;
    }

    return -1;
}

void ActorManager::enterNextTruck()
{
    int pivot_index = this->GetMostRecentTruckSlot();

    for (int i = pivot_index + 1; i < m_free_actor_slot; i++)
    {
        if (m_actors[i] && m_actors[i]->ar_sim_state != Actor::SimState::NETWORKED_OK && !m_actors[i]->isPreloadedWithTerrain())
        {
            setCurrentTruck(i);
            return;
        }
    }

    for (int i = 0; i < pivot_index; i++)
    {
        if (m_actors[i] && m_actors[i]->ar_sim_state != Actor::SimState::NETWORKED_OK && !m_actors[i]->isPreloadedWithTerrain())
        {
            setCurrentTruck(i);
            return;
        }
    }

    if (pivot_index >= 0 && m_actors[pivot_index] && m_actors[pivot_index]->ar_sim_state != Actor::SimState::NETWORKED_OK && !m_actors[pivot_index]->isPreloadedWithTerrain())
    {
        setCurrentTruck(pivot_index);
        return;
    }
}

void ActorManager::enterPreviousTruck()
{
    int pivot_index = this->GetMostRecentTruckSlot();

    for (int i = pivot_index - 1; i >= 0; i--)
    {
        if (m_actors[i] && m_actors[i]->ar_sim_state != Actor::SimState::NETWORKED_OK && !m_actors[i]->isPreloadedWithTerrain())
        {
            setCurrentTruck(i);
            return;
        }
    }

    for (int i = m_free_actor_slot - 1; i > pivot_index; i--)
    {
        if (m_actors[i] && m_actors[i]->ar_sim_state != Actor::SimState::NETWORKED_OK && !m_actors[i]->isPreloadedWithTerrain())
        {
            setCurrentTruck(i);
            return;
        }
    }

    if (pivot_index >= 0 && m_actors[pivot_index] && m_actors[pivot_index]->ar_sim_state != Actor::SimState::NETWORKED_OK && !m_actors[pivot_index]->isPreloadedWithTerrain())
    {
        setCurrentTruck(pivot_index);
        return;
    }
}

void ActorManager::setCurrentTruck(int new_truck)
{
    m_prev_player_actor = m_player_actor;
    m_player_actor = new_truck;

    if (m_prev_player_actor >= 0 && m_player_actor >= 0)
    {
        m_sim_controller->ChangedCurrentVehicle(m_actors[m_prev_player_actor], m_actors[m_player_actor]);
    }
    else if (m_prev_player_actor >= 0)
    {
        m_sim_controller->ChangedCurrentVehicle(m_actors[m_prev_player_actor], nullptr);
    }
    else if (m_player_actor >= 0)
    {
        m_sim_controller->ChangedCurrentVehicle(nullptr, m_actors[m_player_actor]);
    }
    else
    {
        m_sim_controller->ChangedCurrentVehicle(nullptr, nullptr);
    }

    this->UpdateSleepingState(0.0f);
}

bool ActorManager::enterRescueTruck()
{
    // rescue!
    // search a rescue truck
    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (m_actors[t] && m_actors[t]->ar_rescuer_flag)
        {
            // go to person mode first
            setCurrentTruck(-1);
            // then to the rescue truck, this fixes overlapping interfaces
            setCurrentTruck(t);
            return true;
        }
    }
    return false;
}

void ActorManager::updateFlexbodiesPrepare()
{
    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (m_actors[t] && m_actors[t]->ar_sim_state < Actor::SimState::LOCAL_SLEEPING)
        {
            m_actors[t]->updateFlexbodiesPrepare();
        }
    }
}

void ActorManager::joinFlexbodyTasks()
{
    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (m_actors[t] && m_actors[t]->ar_sim_state < Actor::SimState::LOCAL_SLEEPING)
        {
            m_actors[t]->joinFlexbodyTasks();
        }
    }
}

void ActorManager::updateFlexbodiesFinal()
{
    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (m_actors[t] && m_actors[t]->ar_sim_state < Actor::SimState::LOCAL_SLEEPING)
        {
            m_actors[t]->updateFlexbodiesFinal();
        }
    }
}

void ActorManager::updateVisual(float dt)
{
    dt *= m_simulation_speed;

    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (!m_actors[t])
            continue;

        // always update the labels
        m_actors[t]->updateLabels(dt);

        if (m_actors[t]->ar_sim_state < Actor::SimState::LOCAL_SLEEPING)
        {
            m_actors[t]->updateVisual(dt);
            m_actors[t]->updateSkidmarks();
            m_actors[t]->updateFlares(dt, (t == m_player_actor));
        }
    }
}

void ActorManager::update(float dt)
{
    m_physics_frames++;

    // do not allow dt > 1/20
    dt = std::min(dt, 1.0f / 20.0f);

    dt *= m_simulation_speed;

    dt += m_dt_remainder;
    m_physics_steps = dt / PHYSICS_DT;
    m_dt_remainder = dt - (m_physics_steps * PHYSICS_DT);
    dt = PHYSICS_DT * m_physics_steps;

    gEnv->mrTime += dt;

    this->SyncWithSimThread();

    this->UpdateSleepingState(dt);

    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (!m_actors[t])
            continue;

        m_actors[t]->handleResetRequests(dt);
        m_actors[t]->updateAngelScriptEvents(dt);

#ifdef USE_ANGELSCRIPT
        if (m_actors[t]->ar_vehicle_ai && (m_actors[t]->ar_vehicle_ai->IsActive()))
            m_actors[t]->ar_vehicle_ai->update(dt, 0);
#endif // USE_ANGELSCRIPT

        switch (m_actors[t]->ar_sim_state)
        {
        case Actor::SimState::NETWORKED_OK:
            m_actors[t]->calcNetwork();
            break;

        case Actor::SimState::INVALID:
            break;

        default:
            if (m_actors[t]->ar_sim_state != Actor::SimState::LOCAL_SIMULATED && m_actors[t]->ar_engine)
                m_actors[t]->ar_engine->update(dt, 1);
            if (m_actors[t]->ar_sim_state < Actor::SimState::LOCAL_SLEEPING)
                m_actors[t]->UpdatePropAnimations(dt);
            if (m_actors[t]->ar_uses_networking)
            {
                if (m_actors[t]->ar_sim_state == Actor::SimState::LOCAL_SIMULATED)
                    m_actors[t]->sendStreamData();
                else if (m_actors[t]->ar_sim_state == Actor::SimState::LOCAL_SLEEPING && m_actors[t]->ar_net_timer.getMilliseconds() < 10000)
                // Also send update messages for 'Actor::SimState::LOCAL_SLEEPING' trucks during the first 10 seconds of lifetime
                    m_actors[t]->sendStreamData();
                else if (m_actors[t]->ar_sim_state == Actor::SimState::LOCAL_SLEEPING && m_actors[t]->ar_net_timer.getMilliseconds() - m_actors[t]->ar_net_last_update_time > 5000)
                // Also send update messages for 'Actor::SimState::LOCAL_SLEEPING' trucks periodically every 5 seconds
                    m_actors[t]->sendStreamData();
            }
            break;
        }
    }

    m_simulated_actor = m_player_actor;

    if (m_simulated_actor == -1)
    {
        for (int t = 0; t < m_free_actor_slot; t++)
        {
            if (m_actors[t] && m_actors[t]->ar_sim_state == Actor::SimState::LOCAL_SIMULATED)
            {
                m_simulated_actor = t;
                break;
            }
        }
    }

    if (m_simulated_actor >= 0 && m_simulated_actor < m_free_actor_slot)
    {
        if (m_simulated_actor == m_player_actor)
        {

            m_actors[m_simulated_actor]->updateDashBoards(dt);

#ifdef FEAT_TIMING
            if (m_actors[m_simulated_actor]->statistics)     m_actors[m_simulated_actor]->statistics->frameStep(dt);
            if (m_actors[m_simulated_actor]->statistics_gfx) m_actors[m_simulated_actor]->statistics_gfx->frameStep(dt);
#endif // FEAT_TIMING
        }
        if (!m_actors[m_simulated_actor]->replayStep())
        {
            m_actors[m_simulated_actor]->ForceFeedbackStep(m_physics_steps);
            if (m_sim_thread_pool)
            {
                auto func = std::function<void()>([this]()
                    {
                        this->UpdatePhysicsSimulation();
                    });
                m_sim_task = m_sim_thread_pool->RunTask(func);
            }
            else
            {
                this->UpdatePhysicsSimulation();
            }
        }
    }
}

void ActorManager::windowResized()
{

    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (m_actors[t])
        {
            m_actors[t]->ar_dashboard->windowResized();
        }
    }

}

void ActorManager::prepareShutdown()
{
    this->SyncWithSimThread();
}

Actor* ActorManager::GetPlayerActorInternal()
{
    return this->getTruck(m_player_actor);
}

Actor* ActorManager::getTruck(int number)
{
    if (number >= 0 && number < m_free_actor_slot)
    {
        return m_actors[number];
    }
    return 0;
}

void ActorManager::UpdatePhysicsSimulation()
{
    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (!m_actors[t])
            continue;
        m_actors[t]->preUpdatePhysics(m_physics_steps * PHYSICS_DT);
    }
    if (gEnv->threadPool)
    {
        for (int i = 0; i < m_physics_steps; i++)
        {
            int num_simulated_trucks = 0;
            {
                std::vector<std::function<void()>> tasks;
                for (int t = 0; t < m_free_actor_slot; t++)
                {
                    if (m_actors[t] && (m_actors[t]->ar_update_physics = m_actors[t]->calcForcesEulerPrepare(i == 0, PHYSICS_DT, i, m_physics_steps)))
                    {
                        num_simulated_trucks++;
                        auto func = std::function<void()>([this, i, t]()
                            {
                                m_actors[t]->calcForcesEulerCompute(i == 0, PHYSICS_DT, i, m_physics_steps);
                                if (!m_actors[t]->ar_disable_self_collision)
                                {
                                    m_actors[t]->IntraPointCD()->update(m_actors[t]);
                                    intraTruckCollisions(PHYSICS_DT,
                                        *(m_actors[t]->IntraPointCD()),
                                        m_actors[t]->ar_num_collcabs,
                                        m_actors[t]->ar_collcabs,
                                        m_actors[t]->ar_cabs,
                                        m_actors[t]->ar_intra_collcabrate,
                                        m_actors[t]->ar_nodes,
                                        m_actors[t]->ar_collision_range,
                                        *(m_actors[t]->ar_submesh_ground_model));
                                }
                            });
                        tasks.push_back(func);
                    }
                }
                gEnv->threadPool->Parallelize(tasks);
            }

            for (int t = 0; t < m_free_actor_slot; t++)
            {
                if (m_actors[t] && m_actors[t]->ar_update_physics)
                    m_actors[t]->calcForcesEulerFinal(i == 0, PHYSICS_DT, i, m_physics_steps);
            }

            if (num_simulated_trucks > 1)
            {
                std::vector<std::function<void()>> tasks;
                for (int t = 0; t < m_free_actor_slot; t++)
                {
                    if (m_actors[t] && m_actors[t]->ar_update_physics && !m_actors[t]->ar_disable_actor2actor_collision)
                    {
                        auto func = std::function<void()>([this, t]()
                            {
                                m_actors[t]->InterPointCD()->update(m_actors[t], m_actors, m_free_actor_slot);
                                if (m_actors[t]->ar_collision_relevant)
                                {
                                    interTruckCollisions(PHYSICS_DT,
                                        *(m_actors[t]->InterPointCD()),
                                        m_actors[t]->ar_num_collcabs,
                                        m_actors[t]->ar_collcabs,
                                        m_actors[t]->ar_cabs,
                                        m_actors[t]->ar_inter_collcabrate,
                                        m_actors[t]->ar_nodes,
                                        m_actors[t]->ar_collision_range,
                                        m_actors, m_free_actor_slot,
                                        *(m_actors[t]->ar_submesh_ground_model));
                                }
                            });
                        tasks.push_back(func);
                    }
                }
                gEnv->threadPool->Parallelize(tasks);
            }
        }
    }
    else
    {
        for (int i = 0; i < m_physics_steps; i++)
        {
            int num_simulated_trucks = 0;

            for (int t = 0; t < m_free_actor_slot; t++)
            {
                if (m_actors[t] && (m_actors[t]->ar_update_physics = m_actors[t]->calcForcesEulerPrepare(i == 0, PHYSICS_DT, i, m_physics_steps)))
                {
                    num_simulated_trucks++;
                    m_actors[t]->calcForcesEulerCompute(i == 0, PHYSICS_DT, i, m_physics_steps);
                    m_actors[t]->calcForcesEulerFinal(i == 0, PHYSICS_DT, i, m_physics_steps);
                    if (!m_actors[t]->ar_disable_self_collision)
                    {
                        m_actors[t]->IntraPointCD()->update(m_actors[t]);
                        intraTruckCollisions(PHYSICS_DT,
                            *(m_actors[t]->IntraPointCD()),
                            m_actors[t]->ar_num_collcabs,
                            m_actors[t]->ar_collcabs,
                            m_actors[t]->ar_cabs,
                            m_actors[t]->ar_intra_collcabrate,
                            m_actors[t]->ar_nodes,
                            m_actors[t]->ar_collision_range,
                            *(m_actors[t]->ar_submesh_ground_model));
                    }
                }
            }

            if (num_simulated_trucks > 1)
            {
                BES_START(BES_CORE_Contacters);
                for (int t = 0; t < m_free_actor_slot; t++)
                {
                    if (m_actors[t] && m_actors[t]->ar_update_physics && !m_actors[t]->ar_disable_actor2actor_collision)
                    {
                        m_actors[t]->InterPointCD()->update(m_actors[t], m_actors, m_free_actor_slot);
                        if (m_actors[t]->ar_collision_relevant)
                        {
                            interTruckCollisions(
                                PHYSICS_DT,
                                *(m_actors[t]->InterPointCD()),
                                m_actors[t]->ar_num_collcabs,
                                m_actors[t]->ar_collcabs,
                                m_actors[t]->ar_cabs,
                                m_actors[t]->ar_inter_collcabrate,
                                m_actors[t]->ar_nodes,
                                m_actors[t]->ar_collision_range,
                                m_actors, m_free_actor_slot,
                                *(m_actors[t]->ar_submesh_ground_model));
                        }
                    }
                }
                BES_STOP(BES_CORE_Contacters);
            }
        }
    }
    for (int t = 0; t < m_free_actor_slot; t++)
    {
        if (!m_actors[t])
            continue;
        if (!m_actors[t]->ar_update_physics)
            continue;
        m_actors[t]->postUpdatePhysics(m_physics_steps * PHYSICS_DT);
    }
}

void ActorManager::SyncWithSimThread()
{
    if (m_sim_task)
        m_sim_task->join();
}
