#include <mc_control/ControllerClient.h>

#include <mc_rbdyn/RobotLoader.h>

#include <mc_rtc/config.h>

#include <SpaceVecAlg/Conversions.h>

#include <boost/filesystem.hpp>
namespace bfs = boost::filesystem;

#ifdef MC_RTC_HAS_ROS_SUPPORT
#  include <ros/package.h>
#endif

#include "types.h"

#ifdef WIN32
#  define PLUGIN_EXPORT __declspec(dllexport)
#else
#  define PLUGIN_EXPORT
#endif

#define DEFINE_CALLBACK(VAR, FUNCTION, TYPE) \
using VAR##_t = TYPE;\
VAR##_t VAR = nullptr; \
extern "C"\
{\
  PLUGIN_EXPORT void FUNCTION(VAR##_t cb)\
  {\
    VAR = cb;\
  }\
}

DEFINE_CALLBACK(on_robot_callback, OnRobot, void (*)(const char * id))
DEFINE_CALLBACK(on_robot_body_callback, OnRobotBody, void (*)(const char * id, const char * body, McRtc::PTransform X_0_body))
DEFINE_CALLBACK(on_robot_mesh_callback, OnRobotMesh, void (*)(const char * id, const char * body, const char * name, const char * path, float scale, McRtc::PTransform X_body_visual))
DEFINE_CALLBACK(on_trajectory_vector3d_callback, OnTrajectoryVector3d, void (*)(const char * id, float * data, size_t npoints))
DEFINE_CALLBACK(on_remove_element_callback, OnRemoveElement, void (*)(const char * id, const char * type))

inline mc_rbdyn::RobotModulePtr fromParams(const std::vector<std::string> & p)
{
  mc_rbdyn::RobotModulePtr rm{nullptr};
  if(p.size() == 1)
  {
    rm = mc_rbdyn::RobotLoader::get_robot_module(p[0]);
  }
  if(p.size() == 2)
  {
    rm = mc_rbdyn::RobotLoader::get_robot_module(p[0], p[1]);
  }
  if(p.size() == 3)
  {
    rm = mc_rbdyn::RobotLoader::get_robot_module(p[0], p[1], p[2]);
  }
  if(p.size() > 3)
  {
    mc_rtc::log::warning("Too many parameters provided to load the robot, complain to the developpers of this package");
  }
  return rm;
}

inline bfs::path convertURI(const std::string & uri, [[maybe_unused]] std::string_view default_dir = "")
{
  const std::string package = "package://";
  if(uri.size() >= package.size() && uri.find(package) == 0)
  {
    size_t split = uri.find('/', package.size());
    std::string pkg = uri.substr(package.size(), split - package.size());
    auto leaf = bfs::path(uri.substr(split + 1));
    bfs::path MC_ENV_DESCRIPTION_PATH(mc_rtc::MC_ENV_DESCRIPTION_PATH);
#ifndef __EMSCRIPTEN__
//#  ifndef MC_RTC_HAS_ROS_SUPPORT
#  if 1
    // FIXME Prompt the user for unknown packages
    if(pkg == "jvrc_description")
    {
      pkg = (MC_ENV_DESCRIPTION_PATH / ".." / "jvrc_description").string();
    }
    else if(pkg == "mc_env_description")
    {
      pkg = MC_ENV_DESCRIPTION_PATH.string();
    }
    else if(pkg == "mc_int_obj_description")
    {
      pkg = (MC_ENV_DESCRIPTION_PATH / ".." / "mc_int_obj_description").string();
    }
    else
    {
      pkg = default_dir;
    }
#  else
    //  FIXME Unity Hub and Unity Editor don't have the environment variables we set in the shell for ROS
    pkg = ros::package::getPath(pkg);
#  endif
#else
    pkg = "/assets/" + pkg;
#endif
    return pkg / leaf;
  }
  const std::string file = "file://";
  if(uri.size() >= file.size() && uri.find(file) == 0)
  {
    return bfs::path(uri.substr(file.size()));
  }
  return uri;
}

struct UnityClient : public mc_control::ControllerClient
{
  using mc_control::ControllerClient::ControllerClient;
  using ElementId = mc_control::ElementId;

  void default_impl(const std::string &, const ElementId &) override {}

  void started() override
  {
    received_data = false;
    for(auto & it : seen_)
    {
      it.second.seen = false;
    }
  }

  std::string id2unity(const ElementId& id)
  {
    std::string rid;
    for(const auto & cat : id.category)
    {
      rid += cat;
      rid += "/";
    }
    rid += id.name;
    return rid;
  }

  void category(const std::vector<std::string> & parent, const std::string & category) override {}

  void tag_element(const std::string& id, const std::string& type)
  {
    auto it = seen_.find(id);
    if(it == seen_.end())
    {
      seen_[id] = {type, true};
      return;
    }
    auto & tag = it->second;
    tag.seen = true;
    if(tag.type == type)
    {
      return;
    }
    if(on_remove_element_callback)
    {
      on_remove_element_callback(id.c_str(), tag.type.c_str());
    }
    tag.type = type;
  }

  void trajectory(const ElementId & id,
                  const std::vector<Eigen::Vector3d> & points,
                  const mc_rtc::gui::LineConfig & /* config */) override
  {
    if(!on_trajectory_vector3d_callback)
    {
      return;
    }
    std::string tid = id2unity(id);
    tag_element(tid, "trajectory");
    float_buffer.resize(3 * points.size());
    size_t i = 0;
    for(const auto & p : points)
    {
      float_buffer[3 * i + 0] = static_cast<float>(p.x());
      float_buffer[3 * i + 1] = static_cast<float>(p.y());
      float_buffer[3 * i + 2] = static_cast<float>(p.z());
      i += 1;
    }
    on_trajectory_vector3d_callback(tid.c_str(), float_buffer.data(), points.size());
  }

  void robot(const ElementId & id,
             const std::vector<std::string> & parameters,
             const std::vector<std::vector<double>> & q,
             const sva::PTransformd & posW) override
  {
    received_data = true;
    received_data_once = true;
    if(!on_robot_callback)
    {
      return;
    }
    std::string rid = id2unity(id);
    tag_element(rid, "robot");
    if(!robots.count(rid) || modules[rid]->parameters() != parameters)
    {
      modules[rid] = fromParams(parameters);
      if(modules[rid])
      {
        robots[rid] = mc_rbdyn::loadRobot(*modules[rid]);
      }
    }
    auto robots_ptr = robots[rid];
    if(!robots_ptr)
    {
      return;
    }
    auto & robot = robots_ptr->robot();
    robot.mbc().q = q;
    robot.posW(posW);
    on_robot_callback(rid.c_str());
    if(!on_robot_body_callback || !on_robot_mesh_callback)
    {
      return;
    }
    const auto & bodies = robot.mb().bodies();
    const auto & robot_visuals = robot.module()._visual;
    for(size_t bIdx = 0; bIdx < bodies.size(); ++bIdx)
    {
      const auto & b = bodies[bIdx];
      const auto & X_0_b = robot.mbc().bodyPosW[bIdx];
      on_robot_body_callback(rid.c_str(), b.name().c_str(), McRtc::ToUnity(X_0_b));
      if(!robot_visuals.count(b.name()))
      {
        continue;
      }
      size_t i = 0;
      const auto & visuals = robot_visuals.at(b.name());
      for(const auto & v : visuals)
      {
        std::string name = fmt::format("{}_visual", b.name());
        if(visuals.size() > 1)
        {
          name = fmt::format("{}_{}", name, ++i);
        }
        using Geometry = rbd::parsers::Geometry;
        switch(v.geometry.type)
        {
          case Geometry::MESH:
            mesh_callback(rid, b.name(), name, robot.module().path, v);
            break;
          default:
            break;
        }
      }
    }
  }

  void mesh_callback(const std::string & rid,
                     const std::string & body,
                     const std::string & name,
                     const std::string & mesh_path,
                     const rbd::parsers::Visual & visual)
  {
    const auto & mesh = boost::get<rbd::parsers::Geometry::Mesh>(visual.geometry.data);
    std::string path = convertURI(mesh.filename, mesh_path).string();
    on_robot_mesh_callback(rid.c_str(), body.c_str(), name.c_str(), path.c_str(), static_cast<float>(mesh.scale),
                           McRtc::ToUnity(visual.origin));
  }

  void stopped() override
  {
    for(const auto & it : seen_)
    {
      if(!it.second.seen && on_remove_element_callback)
      {
        on_remove_element_callback(it.first.c_str(), it.second.type.c_str());
      }
    }
  }

  using mc_control::ControllerClient::run;

  std::map<std::string, mc_rbdyn::RobotModulePtr> modules;
  std::map<std::string, std::shared_ptr<mc_rbdyn::Robots>> robots;
  struct ElementSeen
  {
    std::string type;
    bool seen;
  };
  std::map<std::string, ElementSeen> seen_;
  bool received_data = false;
  bool received_data_once = false;
  std::vector<float> float_buffer;
};

static std::unique_ptr<UnityClient> client;
static std::string host = "localhost";
static std::mutex client_mutex;
static std::vector<char> buffer(1024 * 1024);
static auto last_received = std::chrono::system_clock::now();

extern "C"
{
  PLUGIN_EXPORT void CreateClient(const char * host_ptr)
  {
    std::unique_lock<std::mutex> lock(client_mutex);
    host = host_ptr;
    if(host.empty())
    {
      host = "localhost";
    }
    client = std::make_unique<UnityClient>(fmt::format("tcp://{}:4242", host), fmt::format("tcp://{}:4343", host), 1.0);
    last_received = std::chrono::system_clock::now();
  }

  PLUGIN_EXPORT void UpdateClient()
  {
    std::unique_lock<std::mutex> lock(client_mutex);
    if(client)
    {
      client->run(buffer, last_received);
      #ifdef WIN32
      if(!client->received_data && client->received_data_once)
      {
        lock.unlock();
        CreateClient(host.c_str());
      }
      #endif
    }
  }

  PLUGIN_EXPORT void StopClient()
  {
    std::unique_lock<std::mutex> lock(client_mutex);
    client.reset(nullptr);
  }
}