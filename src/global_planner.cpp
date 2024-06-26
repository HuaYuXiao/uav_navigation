#include "global_planner.h"

namespace Global_Planning {
    /* 初始化函数 */
    void Global_Planner::init(ros::NodeHandle& nh){
        // 订阅 目标点
        goal_sub = nh.subscribe<geometry_msgs::PoseStamped>("/move_base_simple/goal", 1, &Global_Planner::goal_cb, this);
        // 订阅 无人机状态
        drone_state_sub = nh.subscribe<easondrone_msgs::DroneState>("/easondrone/drone_state", 10, &Global_Planner::drone_state_cb, this);

        // 根据map_input选择地图更新方式
        if(map_input == 0){
            Gpointcloud_sub = nh.subscribe<sensor_msgs::PointCloud2>("/octomap_point_cloud_centers", 10, &Global_Planner::Gpointcloud_cb, this);
        }else if(map_input == 1){
            Lpointcloud_sub = nh.subscribe<sensor_msgs::PointCloud2>("/easondrone/planning/local_pcl", 1, &Global_Planner::Lpointcloud_cb, this);
        }else if(map_input == 2){
            laserscan_sub = nh.subscribe<sensor_msgs::LaserScan>("/easondrone/planning/laser_scan", 1, &Global_Planner::laser_cb, this);
        }

        // 发布 路径指令
        command_pub = nh.advertise<easondrone_msgs::ControlCommand>("/easondrone/control_command", 10);
        // 发布路径用于显示
        path_cmd_pub = nh.advertise<nav_msgs::Path>("/easondrone/planning/path_cmd", 10);

        // 定时器 安全检测
        // safety_timer = nh.createTimer(ros::Duration(2.0), &Global_Planner::safety_cb, this);
        // 定时器 规划器算法执行周期
        mainloop_timer = nh.createTimer(ros::Duration(0.2), &Global_Planner::mainloop_cb, this);
        // 路径追踪循环，快速移动场景应当适当提高执行频率
        track_path_timer = nh.createTimer(ros::Duration(1.0), &Global_Planner::track_path_cb, this);

        // Astar algorithm
        Astar_ptr.reset(new Astar);
        Astar_ptr->init(nh);
        ROS_INFO("Astar init.");

        // 规划器状态参数初始化
        exec_state = EXEC_STATE::WAIT_GOAL;
        odom_ready = false;
        drone_ready = false;
        goal_ready = false;
        sensor_ready = false;
        path_ok = false;
        is_safety = true;
        is_new_path = false;
        cur_id = 0;

        // 初始化发布的指令
        Command_Now.header.stamp = ros::Time::now();
        Command_Now.Mode         = easondrone_msgs::ControlCommand::Idle;
        Command_Now.Command_ID   = 0;
        Command_Now.source       = NODE_NAME;

        desired_yaw = 0.0;
    }

    // 获得新目标点
    void Global_Planner::goal_cb(const geometry_msgs::PoseStampedConstPtr& msg){
            goal_pos << msg->pose.position.x, msg->pose.position.y, _DroneState.position[2];

        // goal rotation
        double z = msg->pose.orientation.z;
        double w = msg->pose.orientation.w;
        desired_yaw = std::atan2(2 * w * z, w * w - z * z);

        // goal velocity
        goal_vel.setZero();

        goal_ready = true;

        ROS_INFO("Get a new goal point: (%f, %f, %f)", goal_pos(0), goal_pos(1), goal_pos(2));
    }

    void Global_Planner::drone_state_cb(const easondrone_msgs::DroneStateConstPtr& msg){
        _DroneState = *msg;

            start_pos << msg->position[0], msg->position[1], msg->position[2];
            start_vel << msg->velocity[0], msg->velocity[1], msg->velocity[2];

        odom_ready = true;

        if (msg->connected && msg->armed){
            drone_ready = true;
        }else{
            drone_ready = false;
        }

        // TODO: Drone_odom is never used
        Drone_odom.header = msg->header;
        Drone_odom.child_frame_id = "base_link";
        Drone_odom.pose.pose.position.x = msg->position[0];
        Drone_odom.pose.pose.position.y = msg->position[1];
        Drone_odom.pose.pose.position.z = msg->position[2];
        Drone_odom.pose.pose.orientation = msg->attitude_q;
        Drone_odom.twist.twist.linear.x = msg->velocity[0];
        Drone_odom.twist.twist.linear.y = msg->velocity[1];
        Drone_odom.twist.twist.linear.z = msg->velocity[2];
    }

    // 根据全局点云更新地图
    // 情况：已知全局点云的场景、由SLAM实时获取的全局点云
    void Global_Planner::Gpointcloud_cb(const sensor_msgs::PointCloud2ConstPtr& msg){
        /* need odom_ for center radius sensing */
        if (!odom_ready){
            return;
        }

        sensor_ready = true;

            // 对Astar中的地图进行更新
            Astar_ptr->Occupy_map_ptr->map_update_gpcl(msg);
            // 并对地图进行膨胀
            Astar_ptr->Occupy_map_ptr->inflate_point_cloud();
    }

    // 根据局部点云更新地图
    // 情况：RGBD相机、三维激光雷达
    // useless. because this P450 is not equipped with D435i
    void Global_Planner::Lpointcloud_cb(const sensor_msgs::PointCloud2ConstPtr& msg){
        /* need odom_ for center radius sensing */
        if (!odom_ready){
            return;
        }

        sensor_ready = true;

        // 对Astar中的地图进行更新（局部地图+odom）
        Astar_ptr->Occupy_map_ptr->map_update_lpcl(msg, Drone_odom);
        // 并对地图进行膨胀
        Astar_ptr->Occupy_map_ptr->inflate_point_cloud();
    }

    // 根据2维雷达数据更新地图
    // 情况：2维激光雷达
    void Global_Planner::laser_cb(const sensor_msgs::LaserScanConstPtr& msg){
        /* need odom_ for center radius sensing */
        if (!odom_ready){
            return;
        }

        sensor_ready = true;

        // 对Astar中的地图进行更新（laser+odom）
        Astar_ptr->Occupy_map_ptr->map_update_laser(msg, Drone_odom);
        // 并对地图进行膨胀
        Astar_ptr->Occupy_map_ptr->inflate_point_cloud();
    }

    void Global_Planner::track_path_cb(const ros::TimerEvent& e){
        if(!path_ok){
            return;
        }

        /*
         if(!is_safety){
             // 若无人机与障碍物之间的距离小于安全距离，则停止执行路径
             // 但如何脱离该点呢？
             ROS_WARN("Drone Position Dangerous! STOP HERE and wait for new goal.");

             Command_Now.header.stamp = ros::Time::now();
             Command_Now.Mode         = easondrone_msgs::ControlCommand::Hold;
             Command_Now.Command_ID   = Command_Now.Command_ID + 1;
             Command_Now.source = NODE_NAME;

             command_pub.publish(Command_Now);

             goal_ready = false;
             exec_state = EXEC_STATE::WAIT_GOAL;

             return;
         }
        */

        is_new_path = false;

        // 抵达终点
        if(cur_id >= Num_total_wp - 1) {
            // the last part of waypoints, move_mode = xyz_pos
            Command_Now.header.stamp = ros::Time::now();
            Command_Now.Mode                                = easondrone_msgs::ControlCommand::Move;
            Command_Now.Command_ID                          = Command_Now.Command_ID + 1;
            Command_Now.source = NODE_NAME;
            Command_Now.Reference_State.Move_mode           = easondrone_msgs::PositionReference::XYZ_POS;
            Command_Now.Reference_State.Move_frame          = easondrone_msgs::PositionReference::ENU_FRAME;
            Command_Now.Reference_State.position_ref[0]     = goal_pos[0];
            Command_Now.Reference_State.position_ref[1]     = goal_pos[1];
            Command_Now.Reference_State.position_ref[2]     = goal_pos[2];
            Command_Now.Reference_State.yaw_ref             = desired_yaw;
            command_pub.publish(Command_Now);

            ROS_INFO("Reach the goal.");

            // 停止执行
            path_ok = false;
            // 转换状态为等待目标
            exec_state = EXEC_STATE::WAIT_GOAL;
            return;
        }

        // 计算距离开始追踪轨迹时间
        tra_running_time = get_time_in_sec(tra_start_time);

        int i = cur_id;

        cout << "Moving to Waypoint: [ " << cur_id << " / "<< Num_total_wp<< " ] "<<endl;
        cout << "Moving to Waypoint:"   << path_cmd.poses[i].pose.position.x  << " m "
                                        << path_cmd.poses[i].pose.position.y  << " m "
                                        << path_cmd.poses[i].pose.position.z  << " m "<<endl;
        // 控制方式如果是走航点，则需要对无人机进行限速，保证无人机的平滑移动
        // 采用轨迹控制的方式进行追踪，期望速度 = （期望位置 - 当前位置）/预计时间；

        Command_Now.header.stamp = ros::Time::now();
        Command_Now.Mode                                = easondrone_msgs::ControlCommand::Move;
        Command_Now.Command_ID                          = Command_Now.Command_ID + 1;
        Command_Now.source = NODE_NAME;
        Command_Now.Reference_State.Move_mode           = easondrone_msgs::PositionReference::TRAJECTORY;
        Command_Now.Reference_State.Move_frame          = easondrone_msgs::PositionReference::ENU_FRAME;
        Command_Now.Reference_State.position_ref[0]     = path_cmd.poses[i].pose.position.x;
        Command_Now.Reference_State.position_ref[1]     = path_cmd.poses[i].pose.position.y;
        Command_Now.Reference_State.position_ref[2]     = path_cmd.poses[i].pose.position.z;
        Command_Now.Reference_State.velocity_ref[0]     = (path_cmd.poses[i].pose.position.x - _DroneState.position[0])/time_per_path;
        Command_Now.Reference_State.velocity_ref[1]     = (path_cmd.poses[i].pose.position.y - _DroneState.position[1])/time_per_path;
        Command_Now.Reference_State.velocity_ref[2]     = (path_cmd.poses[i].pose.position.z - _DroneState.position[2])/time_per_path;
        Command_Now.Reference_State.yaw_ref             = desired_yaw;

        command_pub.publish(Command_Now);

        cur_id = cur_id + 1;
    }

    // 主循环
    void Global_Planner::mainloop_cb(const ros::TimerEvent& e){
        static int exec_num=0;
        exec_num++;

        // 检查当前状态，不满足规划条件则直接退出主循环
        // 此处打印消息与后面的冲突了，逻辑上存在问题
        if(!odom_ready ||
            !drone_ready ||
            !sensor_ready){
            // 此处改为根据循环时间计算的数值
            if(exec_num == 10){
                if(!odom_ready){
                    ROS_WARN("Need Odom.");
                }else if(!drone_ready){
                    ROS_WARN("Drone is not ready.");
                }else if(!sensor_ready){
                    ROS_WARN("Need sensor info.");
                }
                exec_num=0;
            }
            return;
        }else{
            // 对检查的状态进行重置
            odom_ready = false;
            drone_ready = false;
            sensor_ready = false;
        }

        switch (exec_state){
            case WAIT_GOAL:{
                path_ok = false;
                if(!goal_ready){
                    if(exec_num == 10){
                        ROS_INFO("Waiting for a new goal.");
                        exec_num=0;
                    }
                }else{
                    // 获取到目标点后，生成新轨迹
                    exec_state = EXEC_STATE::PLANNING;
                    goal_ready = false;
                }
                break;
            }

            case PLANNING:{
                // 重置规划器
                Astar_ptr->reset();
                // 使用规划器执行搜索，返回搜索结果

                int astar_state;

                // Astar algorithm
                astar_state = Astar_ptr->search(start_pos, goal_pos);

                // 未寻找到路径
                if(astar_state==Astar::NO_PATH){
                    path_ok = false;
                    exec_state = EXEC_STATE::WAIT_GOAL;
//                    ROS_WARN("Planner can't find path!");
                }else{
                    path_ok = true;
                    is_new_path = true;
                    path_cmd = Astar_ptr->get_ros_path();
                    Num_total_wp = path_cmd.poses.size();
                    start_point_index = get_start_point_id();
                    cur_id = start_point_index;
                    tra_start_time = ros::Time::now();
                    exec_state = EXEC_STATE::TRACKING;
                    path_cmd_pub.publish(path_cmd);
                    ROS_INFO("Get a new path!");
                }
                break;
            }

            case TRACKING:{
                // 本循环是1Hz,此处不是很精准
                if(exec_num >= replan_time){
                    exec_state = EXEC_STATE::PLANNING;
                    exec_num = 0;
                }
                break;
            }
        }
    }

    // 【获取当前时间函数】 单位：秒
    float Global_Planner::get_time_in_sec(const ros::Time& begin_time){
        ros::Time time_now = ros::Time::now();
        float currTimeSec = time_now.sec - begin_time.sec;
        float currTimenSec = time_now.nsec / 1e9 - begin_time.nsec / 1e9;
        return (currTimeSec + currTimenSec);
    }

    void Global_Planner::safety_cb(const ros::TimerEvent& e){
        Eigen::Vector3d cur_pos(_DroneState.position[0], _DroneState.position[1], _DroneState.position[2]);

        is_safety = Astar_ptr->check_safety(cur_pos, safe_distance);
    }

    // 选择与当前无人机所在位置最近的点,并从该点开始追踪
    int Global_Planner::get_start_point_id(void){
        int id = 0;
        float distance_to_wp_min = abs(path_cmd.poses[0].pose.position.x - _DroneState.position[0])
                                   + abs(path_cmd.poses[0].pose.position.y - _DroneState.position[1])
                                   + abs(path_cmd.poses[0].pose.position.z - _DroneState.position[2]);

        float distance_to_wp;

        for (int j=1; j<Num_total_wp;j++){
            distance_to_wp = abs(path_cmd.poses[j].pose.position.x - _DroneState.position[0])
                             + abs(path_cmd.poses[j].pose.position.y - _DroneState.position[1])
                             + abs(path_cmd.poses[j].pose.position.z - _DroneState.position[2]);

            if(distance_to_wp < distance_to_wp_min){
                distance_to_wp_min = distance_to_wp;
                id = j;
            }
        }

        //　为防止出现回头的情况，此处对航点进行前馈处理
        if(id + 2 < Num_total_wp){
            id = id + 2;
        }

        return id;
    }
}
