#include "battery_state_updater.h"

BatteryStateUpdater::BatteryStateUpdater(explorer::battery_state *b) {
    loadParameters();
    initializeVariables();
    subscribeToTopics();
    initializeBatteryState();

    logMetadata(); //TODO this makes the tests fail... but maybe it's just because we didn't set the log_path param
    this->b = b;
}

void BatteryStateUpdater::loadParameters() {
    this->b = b; //TODO bad names
    ros::NodeHandle nh_tilde("~");
    if(!nh_tilde.getParam("speed_avg_init", speed_avg_init)) //TODO use config file instead of yaml file for the parameters
        ROS_FATAL("INVALID PARAM");
    if(!nh_tilde.getParam("power_charging", power_charging))
        ROS_FATAL("INVALID PARAM");
    if(!nh_tilde.getParam("power_per_speed", power_per_speed))
        ROS_FATAL("INVALID PARAM");
    if(!nh_tilde.getParam("power_moving_fixed_cost", power_moving_fixed_cost))
        ROS_FATAL("INVALID PARAM");
    if(!nh_tilde.getParam("power_sonar", power_sonar))
        ROS_FATAL("INVALID PARAM");
    if(!nh_tilde.getParam("power_laser", power_laser))
        ROS_FATAL("INVALID PARAM");
    if(!nh_tilde.getParam("power_microcontroller", power_microcontroller))
        ROS_FATAL("INVALID PARAM");
    if(!nh_tilde.getParam("power_basic_computations", power_basic_computations))
        ROS_FATAL("INVALID PARAM");
    if(!nh_tilde.getParam("power_advanced_computations", power_advanced_computations))
        ROS_FATAL("INVALID PARAM");
    if(!nh_tilde.getParam("max_linear_speed", max_speed_linear))
        ROS_FATAL("INVALID PARAM");
    if(!nh_tilde.getParam("maximum_traveling_distance", maximum_traveling_distance))
        ROS_FATAL("INVALID PARAM");
    if(!nh_tilde.getParam("log_path", log_path))
        ROS_FATAL("INVALID PARAM");
    if(!nh_tilde.getParam("robot_prefix", robot_prefix))
        ROS_FATAL("INVALID PARAM");
}

void BatteryStateUpdater::initializeVariables() {
    speed_avg = speed_avg_init;
    last_pose_x = 0, last_pose_y = 0;
    last_traveled_distance = 0;
    total_traveled_distance = 0;
    prev_consumed_energy_A = 0;
}

void BatteryStateUpdater::subscribeToTopics() {
    ros::NodeHandle nh;
    avg_speed_sub = nh.subscribe("avg_speed", 10, &BatteryStateUpdater::avgSpeedCallback, this); //TODO queue lenght
    cmd_vel_sub = nh.subscribe("cmd_vel", 10, &BatteryStateUpdater::cmdVelCallback, this);
    pose_sub = nh.subscribe("amcl_pose", 10, &BatteryStateUpdater::poseCallback, this);
}

void BatteryStateUpdater::initializeBatteryState() {
    b->charging = false;
    b->soc = 1; // (adimensional) // TODO(minor) if we assume that the robot starts fully_charged
    b->remaining_time_charge = 0; // since the robot is assumed to be fully charged when the exploration starts
    b->remaining_distance = maximum_traveling_distance;
    b->remaining_time_run = maximum_traveling_distance * speed_avg_init; //s //TODO(minor) "maximum" is misleading: use "estimated"...
    b->maximum_traveling_distance = maximum_traveling_distance;
    b->fully_charged = true; //TODO assumption
    b->consumed_energy_A = 0;
    b->consumed_energy_B = 0;
}

void BatteryStateUpdater::avgSpeedCallback(const explorer::Speed &msg)
{
    // if the average speed is very low, there is probably something wrong, set it to the value from the config file
    if (msg.avg_speed > speed_avg_init)
        speed_avg = msg.avg_speed;
    else
        speed_avg = speed_avg_init;
}

void BatteryStateUpdater::poseCallback(const geometry_msgs::PoseWithCovarianceStampedConstPtr &pose) {
    mutex_traveled_distance.lock();

    double pose_x = pose->pose.pose.position.x;
    double pose_y = pose->pose.pose.position.y;    
    
    double distance = sqrt( (last_pose_x-pose_x)*(last_pose_x-pose_x) + (last_pose_y-pose_y)*(last_pose_y-pose_y) ); //TODO use hypot
    last_traveled_distance += distance;
    total_traveled_distance += distance;
    
    last_pose_x = pose_x;
    last_pose_y = pose_y;

    mutex_traveled_distance.unlock();
}

void BatteryStateUpdater::cmdVelCallback(const geometry_msgs::Twist &msg)
{
    ROS_DEBUG("Received speed");
    speed_linear = msg.linear.x;
    speed_angular = msg.angular.z;
}

void BatteryStateUpdater::updateBatteryState() { //TODO use visitor
    ROS_INFO("Updating battery state");
    computeElapsedTime();

//    robot_state_manager.getRobotState()->accept(this);
    robot_state::robot_state_t robot_state;
    robot_state = static_cast<robot_state::robot_state_t>(robot_state_manager->getRobotState());

    if(robot_state == robot_state::INITIALIZING) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForBasicComputations();

    } else if(robot_state == robot_state::CHOOSING_ACTION) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();

    } else if(robot_state == robot_state::COMPUTING_NEXT_GOAL) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();
        substractEnergyRequiredForAdvancedComputations();

    } else if(robot_state == robot_state::MOVING_TO_FRONTIER) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();
        substractEnergyRequiredForLocomotion();
        subtractTraveledDistance();

    } else if(robot_state == robot_state::GOING_CHECKING_VACANCY) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();
        substractEnergyRequiredForLocomotion();
        subtractTraveledDistance();

    } else if(robot_state == robot_state::CHECKING_VACANCY) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();

    } else if(robot_state == robot_state::GOING_CHARGING) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();
        substractEnergyRequiredForLocomotion();
        subtractTraveledDistance();

    } else if(robot_state == robot_state::CHARGING) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForBasicComputations();
        rechargeBattery();

    } else if(robot_state == robot_state::CHARGING_COMPLETED) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();

    } else if(robot_state == robot_state::CHARGING_ABORTED) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();

    } else if(robot_state == robot_state::LEAVING_DS) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();
        substractEnergyRequiredForLocomotion();
        //subtractTraveledDistance(); //TODO(IMPORTANT) we should use this, but it complicates a lot the exstimations

    } else if(robot_state == robot_state::GOING_IN_QUEUE) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();
        substractEnergyRequiredForLocomotion();

    } else if(robot_state == robot_state::IN_QUEUE) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForBasicComputations();

    } else if(robot_state == robot_state::AUCTIONING) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();

    } else if(robot_state == robot_state::auctioning_2) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();

    } else if(robot_state == robot_state::exploring_for_graph_navigation) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();

    } else if(robot_state == robot_state::stopped) {
        ;

    } else if(robot_state == robot_state::stuck) {
        ;

    } else if(robot_state == robot_state::auctioning_3) {
        substractEnergyRequiredForKeepingRobotAlive();
        substractEnergyRequiredForSensing();
        substractEnergyRequiredForBasicComputations();

    } else if(robot_state == robot_state::finished) {
        ;

    } else
        ROS_FATAL("INVALID ROBOT STATE");

    if(robot_state != robot_state::CHARGING)
        prev_consumed_energy_A = b->consumed_energy_A;

    updateRemainingUsableDistanceAndRunningTime();
}

//void BatteryStateUpdater::handle(InitializingState *r) { //TODO check how each state consumes energy...
//    substractEnergyRequiredForKeepingRobotAlive();
//    substractEnergyRequiredForBasicComputations();    
//}

//void BatteryStateUpdater::handle(ChoosingActionState *r) {
//    substractEnergyRequiredForKeepingRobotAlive();
//    substractEnergyRequiredForSensing();
//    substractEnergyRequiredForBasicComputations();
//}

//void BatteryStateUpdater::handle(ComputingNextGoalState *r) {
//    substractEnergyRequiredForKeepingRobotAlive();
//    substractEnergyRequiredForSensing();
//    substractEnergyRequiredForBasicComputations();
//    substractEnergyRequiredForAdvancedComputations();
//}

void BatteryStateUpdater::computeElapsedTime() { //TODO add this function to TimeManager instead
    double time_now = time_manager->simulationTimeNow().toSec();
    elapsed_time = time_now - time_last_update;
    time_last_update = time_now;
}

void BatteryStateUpdater::substractEnergyRequiredForKeepingRobotAlive() {
    b->consumed_energy_B += elapsed_time * power_microcontroller;
}

void BatteryStateUpdater::substractEnergyRequiredForSensing() {
    b->consumed_energy_B += elapsed_time * (power_sonar + power_laser);
}

void BatteryStateUpdater::substractEnergyRequiredForBasicComputations() {
    b->consumed_energy_B += elapsed_time * power_basic_computations;
}

void BatteryStateUpdater::substractEnergyRequiredForAdvancedComputations() {
    b->consumed_energy_B += elapsed_time * power_advanced_computations;
}

void BatteryStateUpdater::substractEnergyRequiredForLocomotion() {
    b->consumed_energy_A += elapsed_time * (power_per_speed * speed_linear + power_moving_fixed_cost);
}

void BatteryStateUpdater::subtractTraveledDistance() {
    b->remaining_distance -= last_traveled_distance;
    last_traveled_distance = 0;
}

void BatteryStateUpdater::rechargeBattery() {
    ROS_INFO("Recharging battery");
    double ratio_A = -1, ratio_B = -1;

    if(b->consumed_energy_A < 0 && b->consumed_energy_B < 0) {
        ROS_FATAL("this should not happen...");
    }

    if(b->consumed_energy_A <= 0) {
        ratio_A = 0.0;
        ratio_B = 1.0;
        b->consumed_energy_A = 0;
        ROS_ERROR("this should not happen");
    }   
    else if(b->consumed_energy_B <= 0) {
        ratio_A = 1.0;
        ratio_B = 0.0;
        b->consumed_energy_B = 0;
        ROS_ERROR("this should not happen");
    }
    else {
        ratio_A = b->consumed_energy_A / (b->consumed_energy_A + b->consumed_energy_B);
        ratio_B = b->consumed_energy_B / (b->consumed_energy_A + b->consumed_energy_B);
    }
    
    if(ratio_A < 0 || ratio_A > 1 || ratio_B < 0 || ratio_B > 1 || fabs(ratio_A + ratio_B - 1.0) > 0.01 ) //TODO this sanity check should be useless
        ROS_FATAL("strange ratio");
    
    b->consumed_energy_A -= ratio_A * power_charging * elapsed_time;
    b->consumed_energy_B -= ratio_B * power_charging * elapsed_time;
}

void BatteryStateUpdater::updateRemainingUsableDistanceAndRunningTime() {
    ROS_FATAL("MISSING");
    if(b->consumed_energy_A <=0 && b->consumed_energy_B <= 0)
    {
        ROS_INFO("Recharging completed");
         
        b->consumed_energy_A = 0;
        b->consumed_energy_B = 0;
        
        // Set battery state to its maximum values 
        b->remaining_distance = maximum_traveling_distance;
        b->remaining_time_charge = 0;
        b->remaining_time_run = b->remaining_distance * speed_avg;
        b->soc = 1;
        
        robot_state_manager->lockRobotState();
        robot_state::robot_state_t robot_state;
        robot_state = static_cast<robot_state::robot_state_t>(robot_state_manager->getRobotState());
        if(robot_state == robot_state::CHARGING)
            robot_state_manager->setRobotState(robot_state::CHARGING_COMPLETED);
        robot_state_manager->unlockRobotState();
        
    } else {
        if(b->consumed_energy_A < 0) 
            ROS_ERROR("this shouldn't happend");

        else if(b->consumed_energy_B < 0)
            ROS_ERROR("this shouldn't happend");

        b->remaining_time_charge = (b->consumed_energy_A + b->consumed_energy_B) / power_charging ;
        b->remaining_distance = ( (prev_consumed_energy_A - b->consumed_energy_A) / prev_consumed_energy_A ) * maximum_traveling_distance;
        if(b->remaining_distance > maximum_traveling_distance) {
            ROS_ERROR("state.remaining_distance > maximum_traveling_distance");
            b->remaining_distance = maximum_traveling_distance;
        }
        b->remaining_time_run = b->remaining_distance * speed_avg;
        b->soc = b->remaining_distance / maximum_traveling_distance;


    }
}

void BatteryStateUpdater::setTimeManager(TimeManagerInterface *time_manager) {
    this->time_manager = time_manager;
}

void BatteryStateUpdater::setRobotStateManager(RobotStateManagerInterface *robot_state_manager) {
    this->robot_state_manager = robot_state_manager;
}

void BatteryStateUpdater::createLogDirectory() {
    /* Create directory */
    log_path = log_path.append("/energy_mgmt");
    log_path = log_path.append(robot_name);
    boost::filesystem::path boost_log_path(log_path.c_str());
    if (!boost::filesystem::exists(boost_log_path))
    {
        ROS_INFO("Creating directory %s", log_path.c_str());
        try
        {
            if (!boost::filesystem::create_directories(boost_log_path))
            {
                ROS_ERROR("Cannot create directory %s: aborting node...", log_path.c_str());
                exit(-1);
            }
        }
        catch (const boost::filesystem::filesystem_error &e)
        {
            ROS_ERROR("Cannot create path %saborting node...", log_path.c_str());
            exit(-1);
        }
    }
    else
    {
        ROS_INFO("Directory %s already exists: log files will be saved there", log_path.c_str());
    }
}

void BatteryStateUpdater::logMetadata() {
    /* Create file names */
    log_path = log_path.append("/");
    info_file = log_path + std::string("metadata_battery.csv");

    fs_info.open(info_file.c_str(), std::fstream::in | std::fstream::app | std::fstream::out);
    fs_info << "#power_sonar, power_laser, power_basic_computations, power_advanced_computations, power_microcontroller, power_moving_fixed_cost, power_per_speed, power_charging,max_linear_speed,initial_speed_avg" << std::endl;
    fs_info << power_sonar << "," << power_laser << "," << power_basic_computations << "," << power_advanced_computations << "," << power_microcontroller << "," << power_moving_fixed_cost << "," << power_per_speed << "," << power_charging << "," << max_speed_linear << "," << speed_avg_init << std::endl;
    fs_info.close();
}
