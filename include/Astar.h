#ifndef _ASTAR_H
#define _ASTAR_H

#include "plan_env/occupy_map.h"

#define NODE_NAME "Global_Planner"

namespace Global_Planning{

#define IN_CLOSE_SET 'a'
#define IN_OPEN_SET 'b'
#define NOT_EXPAND 'c'
//#define inf 1 >> 30

class Node{
    public:
        /* -------------------- */
        Eigen::Vector3i index;
        Eigen::Vector3d position;
        double g_score, f_score;
        Node* parent;
        char node_state;

        Node(){
          parent = NULL;
          node_state = NOT_EXPAND;
        }
        ~Node(){};
};
typedef Node* NodePtr;

// TODO 为什么这么麻烦，不能直接比较吗
class NodeComparator0{
    public:
        bool operator()(NodePtr node1, NodePtr node2){
          return node1->f_score > node2->f_score;
        }
};

template <typename T>
struct matrix_hash0 : std::unary_function<T, size_t>{
    std::size_t operator()(T const& matrix) const{
        size_t seed = 0;
        for (size_t i = 0; i < matrix.size(); ++i){
            auto elem = *(matrix.data() + i);
            seed ^= std::hash<typename T::Scalar>()(elem) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};


class NodeHashTable0{
  private:
      /* data */
      std::unordered_map<Eigen::Vector3i, NodePtr, matrix_hash0<Eigen::Vector3i>> data_3d_;

  public:
      NodeHashTable0(/* args */){}
      ~NodeHashTable0(){}
      void insert(Eigen::Vector3i idx, NodePtr node){
        data_3d_.insert(std::make_pair(idx, node));
      }

      NodePtr find(Eigen::Vector3i idx){
        auto iter = data_3d_.find(idx);
        return iter == data_3d_.end() ? NULL : iter->second;
      }

      void clear(){
        data_3d_.clear();
      }
};


class Astar{
    private:
        // 备选路径点指针容器
        std::vector<NodePtr> path_node_pool_;
        // 使用节点计数器、迭代次数计数器
        int use_node_num_;
        int iter_num_;
        // 扩展的节点
        NodeHashTable0 expanded_nodes_;
        // open set （根据规则已排序好）
        std::priority_queue<NodePtr, std::vector<NodePtr>, NodeComparator0> open_set_;
        // 最终路径点容器
        std::vector<NodePtr> path_nodes_;  

        // tie breaker
        double tie_breaker_;

        /* ---------- record data ---------- */
        // 目标点
        Eigen::Vector3d goal_pos;

        // 地图相关
        std::vector<int> occupancy_buffer_;  

        // TODO defined twice!
        Eigen::Vector3d origin_;
        Eigen::Vector3d map_size_3d_;
        bool has_global_point;

        // 辅助函数
        Eigen::Vector3i posToIndex(Eigen::Vector3d pt);
        void indexToPos(Eigen::Vector3i id, Eigen::Vector3d &pos);
        void retrievePath(NodePtr end_node);

        // 搜索启发函数，三种形式，选用其中一种即可
        double getDiagHeu(Eigen::Vector3d x1, Eigen::Vector3d x2);
        double getManhHeu(Eigen::Vector3d x1, Eigen::Vector3d x2);
        double getEuclHeu(Eigen::Vector3d x1, Eigen::Vector3d x2);

    public:
        Astar(){}
        ~Astar();

        typedef std::shared_ptr<Astar> Ptr;

        enum{
          REACH_END = 1,
          NO_PATH = 2
        };

        // 占据图类
        Occupy_map::Ptr Occupy_map_ptr;

        // 重置
        void reset();
        // 初始化
        void init(ros::NodeHandle& nh);

        // 检查安全性
        bool check_safety(Eigen::Vector3d &cur_pos, double safe_distance);
        // 搜索
        int search(Eigen::Vector3d start_pt, Eigen::Vector3d end_pt);
        // 返回路径
        std::vector<Eigen::Vector3d> getPath();
        // 返回ros消息格式的路径
        nav_msgs::Path get_ros_path();
        // 返回访问过的节点
        std::vector<NodePtr> getVisitedNodes();
};
}

#endif
