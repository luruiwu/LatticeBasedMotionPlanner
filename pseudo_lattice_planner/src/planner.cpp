#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <pseudo_lattice_planner/planner.h>



/********************************************************************
 * Planner implementation. 
 ********************************************************************/

namespace pseudo_lattice_planner {

#define ROUNDED_ZERO 1e-6

int Hasher::num_gridcells_ = 0;
int Hasher::num_orientations_ = 0;
int Hasher::num_vels_phi_ = 0;
int Hasher::num_vels_x_ = 0;

/********************************************************************
 * Constructor
 ********************************************************************/
LatticePlanner::LatticePlanner(std::string name, 
                           costmap_2d::Costmap2DROS *costmap) {
    ROS_INFO("LatticePlanner::LatticePlanner");
    ros::NodeHandle private_nh("~/" + name);
    vel_path_pub_ = 
        private_nh.advertise<nav_msgs::Path>("plan", 1);
    expanded_paths_pub_ = 
        private_nh.advertise<nav_msgs::Path>("expanded_paths", 1);
    current_node_pub_ = 
        private_nh.advertise<geometry_msgs::PoseStamped>("current_node", 1);
    new_node_pub_ = 
        private_nh.advertise<geometry_msgs::PoseStamped>("new_node", 1);
    all_expanded_pub_ = 
        private_nh.advertise<nav_msgs::Path>("all_expanded", 1);

    path_pub_ = private_nh.advertise<nav_msgs::Path>("path", 1);

    //CostFactors cost_factors;
    private_nh.param("lethal_cost", cost_factors_.lethal_cost,
                         (int)costmap_2d::INSCRIBED_INFLATED_OBSTACLE);
    private_nh.param("time_cost_factor", cost_factors_.time_cost, 200.0);
    private_nh.param("step_cost_factor", cost_factors_.step_cost, 80.0);
    private_nh.param("rotation_cost_factor", cost_factors_.rotation_cost, 5.0);
    private_nh.param("environment_cost_factor", cost_factors_.environment_cost, 1.0);
    
    double collision_check_time_res;
    private_nh.param("allow_unknown", allow_unknown_, false);
    private_nh.param("time_resolution", time_resolution_, 0.5);
    private_nh.param("collision_check_time_resolution", collision_check_time_res, 0.1);
    private_nh.param("planning_timeout", planning_timeout_, 5.0);
    private_nh.param("heuristic_dijkstra", use_dijkstra_, false);
    private_nh.param("publish_expanded", publish_expanded_, false);
    private_nh.param("debug", debug_, false);

    private_nh.param("min_vel_x", motion_constraints_.min_vel_x, 0.0);
    private_nh.param("max_vel_x", motion_constraints_.max_vel_x, 0.4);
    private_nh.param("acceleration_x", motion_constraints_.acc_x, 0.8);
    private_nh.param("min_vel_phi", motion_constraints_.min_vel_phi, -0.8);
    private_nh.param("max_vel_phi", motion_constraints_.max_vel_phi, 0.8);
    private_nh.param("acceleration_phi", motion_constraints_.acc_phi, 1.6);
    private_nh.param("heuristic_inflation", eps_, 3.0);

    costmap_ = costmap;
    double resolution = costmap_->getCostmap()->getResolution();
    
    map_index_check_time_incr_ = 0.5*resolution/motion_constraints_.max_vel_x;
    
    // for calculating trajectory costs
    cost_calc_ = new CostManager(costmap_, motion_constraints_, cost_factors_);
    
    // for discretizing states
    discretizer_ = new StateDiscretizer(costmap_, motion_constraints_, time_resolution_);
    
    // set up hasher for state_map
    int num_gridcells;
    int num_orientations;
    int num_vels_x;
    int num_vels_phi;
    discretizer_->getLimits(num_gridcells, num_orientations, num_vels_x, num_vels_phi);
    Hasher::setLimits(num_gridcells, num_orientations, num_vels_x, num_vels_phi);
    
    // motion primitive generator
    primgen_ = new LatticePrims(motion_constraints_);
    
    // heuristic calculator
    heuristic_calc_ = new Heuristics(costmap_, cost_factors_, motion_constraints_, time_resolution_);
    heuristic_calc_->initPublisher("pseudo_lattice_planner/heuristics", 100);
    
    // set up Q-learning thing
    // alpha, gamma
    QT_ = new QStuff::QTable(0.2, 0.5);
    QT_->loadTable("/home/grogan/Qtable_vals.txt");
    QSD_ = new QStuff::QStateDiscretizer(-0.08887, 0.82106,
                                          //1.44389, 0.31415,
                                          4.54869, 0.65246,
                                          eps_);


    open_list_ = NULL;
    incons_list_ = NULL;
    all_expanded_.rehash(1000000); //million good?
    all_expanded_.reserve(1000000);

    start_state_ = NULL;
    goal_state_ = NULL;
    search_iteration_ = 0;
    call_number_ = 0;
}

/********************************************************************
 * Destructor
 ********************************************************************/
LatticePlanner::~LatticePlanner() {
    ROS_INFO("LatticePlanner::~LatticePlanner");
    reset();
    delete open_list_;
    delete incons_list_;
    delete discretizer_;
    delete primgen_;
    delete heuristic_calc_;
    delete cost_calc_;
    delete QT_;
    delete QSD_;
}

/********************************************************************
 * Free up containers.
 * Deleting states here.
 ********************************************************************/
void LatticePlanner::reset() {
    //ROS_INFO("LatticePlanner::reset");
    if (open_list_ != NULL) {
        if (!open_list_->isEmpty()) {
            open_list_->makeEmpty();
        }
        //delete open_list_;
        //open_list_ = NULL;
    }
    if (incons_list_ != NULL) {
        if (!incons_list_->isEmpty()) {
            incons_list_->makeEmpty();
        }
        //delete incons_list_;
        //incons_list_ = NULL;
    }
    state_map::iterator it;
    for (it=all_expanded_.begin(); it != all_expanded_.end(); it++) {
        if (it->second != NULL) {
            delete it->second;
        }
        else
            ROS_WARN("NULL state in reset()");
    }
    all_expanded_.clear();

    expanded_paths_.poses.clear();

    //clearRobotFootprint();
}

/********************************************************************
 * Timer callback. This is not set up currently. Will ultimately be 
 * used to set a time limit on planning.
 ********************************************************************/
void LatticePlanner::plannerTimeoutCallback(const ros::TimerEvent &event) {
    ROS_INFO("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    ROS_INFO("LatticePlanner::plannerTimeoutCallback");
    ROS_INFO("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    planning_timed_out_ = true;
}

/********************************************************************
 * Clear the ole robot footprint. Haven't really validated this...
 ********************************************************************/
void LatticePlanner::clearRobotFootprint() {
    //ROS_INFO("LatticePlanner::clearRobotFootprint");
    std::vector<geometry_msgs::Point> footprint = costmap_->getRobotFootprint();
    costmap_->getCostmap()->setConvexPolygonCost(footprint, costmap_2d::FREE_SPACE);
}

/********************************************************************
 * Allocate memory for a new state and assign default values.
 * Add to the state_map of expanded states.
 ********************************************************************/
State* LatticePlanner::createState(State state) {
    State *new_state = new State;
    new_state->stateID = state.stateID;
    new_state->heapIndex = 0;
    new_state->listElement = NULL;
    new_state->iterationClosed = 0;
    new_state->callNumberAccessed = call_number_;
    new_state->numExpands = 0;
    new_state->state_i = state.state_i;
    new_state->pose = state.pose;
    new_state->vel = state.vel;
    new_state->trajectory = state.trajectory;
    new_state->parent = state.parent;
    new_state->costToBestSucc = MAX;
    new_state->g = MAX;
    new_state->v = MAX;
    new_state->h = heuristic_calc_->getHeuristic(new_state, goal_pose_);

    std::pair<DiscreteState, State*> pair(new_state->state_i, new_state);
    all_expanded_.insert(pair);

    return new_state;
}

/********************************************************************
 * Return a state pointer for a state with the key state_i. If such a
 * state has already been created, it will be in the state_map. 
 * Otherwise, create it.
 ********************************************************************/
State* LatticePlanner::getState(State state) {
    state_map::const_iterator it = all_expanded_.find(state.state_i);
    if (it != all_expanded_.end()) { // already exists
        //ROS_INFO("getState: already exists");
        return it->second;
    } else {                        // doesn't exist, create it
        //ROS_INFO("getState: creating state");
        return createState(state);
    }
}

/********************************************************************
 * Verify that the trajectory described by indices does not pass
 * through an obstacle.
 ********************************************************************/
bool LatticePlanner::isValidTrajectory(std::vector<unsigned int> indices) {
    for (int i=0; i<indices.size(); i++) {
        unsigned int mx, my;
        costmap_->getCostmap()->indexToCells(indices[i], mx, my);
        unsigned int cell_cost = costmap_->getCostmap()->getCost(mx, my);
        if (cell_cost >= cost_factors_.lethal_cost) {
            //ROS_INFO("cell_cost = %d", cell_cost);
            return false;
        }
    }
    return true;
}

/********************************************************************
 * Generate the set of feasible states that can be reached from 
 * current state
 ********************************************************************/
std::vector<State> LatticePlanner::getNextStates(State *current) {
    std::vector<State> ret;
    std::vector<Velocity> prims = primgen_-> 
        getReachableVelocities(current->vel, time_resolution_);
    for (int i=0; i<prims.size(); i++) {
        State new_state;
        
        Velocity travel_vel((current->vel.vel_x + prims[i].vel_x)/2,
                            (current->vel.vel_phi + prims[i].vel_phi)/2);

        Pose next_pose = primgen_->getNextPose(current->pose,
                                               travel_vel,
                                               time_resolution_);

        DiscreteState state_i = discretizer_->discretizeState(next_pose, prims[i]);

        // check if valid trajectory
        std::vector<unsigned int> indices = 
            primgen_->getTrajectoryIndices(current, state_i, travel_vel,
                                           discretizer_, map_index_check_time_incr_,
                                           time_resolution_);

        if (!isValidTrajectory(indices) || !state_i.in_map) {
            continue;
        }

        // these have already been computed so we might as well
        // pass them along
        new_state.stateID = Hasher::getHash(state_i);
        new_state.state_i = state_i;
        new_state.pose = next_pose;
        new_state.vel = prims[i];
        new_state.trajectory = indices;
        new_state.parent = current;
    
        ret.push_back(new_state);
    }
    return ret;
}

/********************************************************************
 * Improve on the current path. Reference ARA* paper for more.
 ********************************************************************/
int LatticePlanner::improvePath(double cur_eps, State *&best, bool &goal_found,
                                 double end_time) {
    ROS_INFO("LatticePlanner::improvePath");
    int expands = 0;
    State *current = NULL;
    double goal_key = goal_state_->g;
    double min_key = open_list_->getMinKey();
    double old_key = min_key;

    while (!open_list_->isEmpty() && min_key < MAX && goal_key > min_key &&
           (double)clock()/CLOCKS_PER_SEC < end_time) {
        if (expands%50000 == 0) {
            ROS_INFO("%f < %f", (double)clock()/CLOCKS_PER_SEC, end_time);
        }
        // pop heap
        current = open_list_->deleteMinElement();
        if (best == NULL) {
            best = current;
        } else if (current->h < best->h) {
            best = current;
        }
        
        if (current->listElement != NULL) {
            ROS_ERROR("state is already in incons_list_");
        }
        if (current->v == current->g) {
            ROS_ERROR("consistent state being expanded");
        }

        // state is now closed
        current->iterationClosed = search_iteration_;
        current->v = current->g;
        expands++;
        current->numExpands++;

        // get successor states
        std::vector<State> next_states = getNextStates(current);
        for (int i=0; i<next_states.size(); i++) {
            if (next_states[i].parent == NULL) { // hasn't happened yet
                ROS_ERROR("next_states[%d].parent == NULL", i);
            }

            State *succ = getState(next_states[i]);
            // if state is the goal, set the goal->parent pointer accordingly
            if (succ->state_i == goal_state_->state_i && succ->parent == NULL) {
                ROS_INFO("found goal");
                succ->parent = current;
                goal_found = true;
            }
            // if state is the start, ignore - can't be improved
            if (succ->state_i == start_state_->state_i) {
                continue;
            }

            if (succ->parent == NULL) { // shouldn't happen
                ROS_ERROR("succ.parent == NULL");
                ROS_ERROR("succ->stateID = %d", succ->stateID);
            }

            double cost = cost_calc_->getTrajectoryCost(succ);

            // improve state if possible
            if (succ->g > current->v + cost) {
                succ->g = current->v + cost;
                succ->parent = current;

                // if state was not previously closed, heap it
                // else it belongs in inconsistent list (if not already there)
                if (succ->iterationClosed != search_iteration_) {
                    double key = succ->g + cur_eps*succ->h;

                    if (succ->heapIndex != 0) {
                        open_list_->updateElement(succ, key);
                    } else {
                        open_list_->insertElement(succ, key);
                    }
                } else if (succ->listElement == NULL) {
                    incons_list_->insertElement(succ);
                }
            }
        }

        // reset keys
        old_key = min_key;
        min_key = open_list_->getMinKey();
        goal_key = goal_state_->g;

        // debugging stuff set in the params file
        if (publish_expanded_) {
            pseudo_lattice_planner::Path vel_path = retracePath(current);
            std::vector<geometry_msgs::PoseStamped> poses = vel_path.poses;
            expanded_paths_.poses.insert(expanded_paths_.poses.end(),
                    poses.begin(), poses.end());
            std::reverse(poses.begin(), poses.end());
            expanded_paths_.poses.insert(expanded_paths_.poses.end(),
                    poses.begin(), poses.end());
            if (debug_) {
                //ROS_INFO("expanded_paths_ size: %d", expanded_paths_.poses.size());
                expanded_paths_.header.frame_id = costmap_->getGlobalFrameID();
                //expanded_paths_.header.stamp = ros::Time::now();
                expanded_paths_pub_.publish(expanded_paths_);
                ROS_INFO("goal_key = %.3f", goal_key);
                ROS_INFO("min_key = %.3f", min_key);
                ROS_INFO("old_key = %.3f", old_key);
                std::string input;
                std::cout << "press enter to continue, c + enter to finish planning without interrupt: ";

                std::getline(std::cin, input);
                if(input.compare("c") == 0)
                    debug_ = false;
            }
        }
        ros::spinOnce();
    }
    if (open_list_->isEmpty()) {
        ROS_INFO("improvePath: open list empty");
        return -1;
    } else if (min_key >= MAX) {
        ROS_INFO("improvePath: min_key >= MAX");
        return -1;
    } else if (goal_key <= min_key) {
        ROS_INFO("improvePath: goal_key <= min_key");
        return 0;
    } else {
        ROS_INFO("improvePath: timed out");
        return 1;
    }
}

/********************************************************************
 * Main function. Need to set up planning timeout and set up a way 
 * of signaling the path executer to begin execution without exiting.
 * Or at least without losing information. That might mean a more
 * sophisticated method of evaluating when to reset
 ********************************************************************/
bool LatticePlanner::getPath(geometry_msgs::PoseStamped start,
                             geometry_msgs::PoseStamped goal,
                     std::vector<geometry_msgs::PoseStamped> &path) {
    ROS_INFO("LatticePlanner::getPath");
    //call_number_++;
    // clear containers
    reset();
    // new containers
    open_list_ = new Heap;
    incons_list_ = new List;
    
    if(!initStartAndGoal(start, goal)) {
        return false;
    }

    call_number_++;

    // main search loop setup
    State *current_best = NULL;
    bool found_goal = false;
    double cur_eps = eps_;



    TimingInfo timing_info(time_resolution_);
    // set heuristic inside clock
    if (use_dijkstra_) {
        heuristic_calc_->dijkstraHeuristic(goal_state_);
    } else {
        heuristic_calc_->diagDistanceHeuristic(goal_state_);
    }
    heuristic_calc_->publishHeuristic();

    // main loop
    search_iteration_ = 1;
    int ret = improvePath(cur_eps, current_best, 
            found_goal, (double)clock()/CLOCKS_PER_SEC+planning_timeout_);

    if (ret == -1) {
        ROS_WARN("bad");
        return false;
    }
    if (ret == 1 || !found_goal) {
        ROS_WARN("timed out on first search");
        return false;
    }


    
    int plan_len = publishPlan(goal_state_);
    timing_info.update(plan_len);
    double reward = 0.0;
    double bounded_eps = getBoundedEps(cur_eps);
    printStuff(timing_info, cur_eps, bounded_eps, reward);

    QStuff::QState q = QSD_->discretize(timing_info.plan_time_, 
                                        //timing_info.exec_time_, 
                                        open_list_->cur_size, cur_eps);
    int action = QT_->getAction(q);
    ROS_INFO("Action %d", action);

    while (action != ACTION_E && (cur_eps > 1+ROUNDED_ZERO || ret == 1)) {
        double action_time = (double)clock()/CLOCKS_PER_SEC + 
                             QSD_->indexToAction(action);
        while ((double)clock()/CLOCKS_PER_SEC < action_time && 
               (cur_eps > 1+ROUNDED_ZERO || ret == 1)) {
            if (ret == 0) {
                search_iteration_++;
                cur_eps -= 0.4;
                buildOpenList(cur_eps);
            }
            ret = improvePath(cur_eps, current_best, 
                              found_goal, action_time);
            plan_len = publishPlan(goal_state_);
        }
        reward = timing_info.update(plan_len);
        QStuff::QState q_new = QSD_->discretize(timing_info.plan_time_, 
                                        //timing_info.exec_time_, 
                                        open_list_->cur_size, cur_eps);
        QT_->update(q, q_new, action, reward);
        ROS_INFO("Updated table value: %.3f", QT_->getTableVal(q, action));
        QT_->saveTable("/home/grogan/Qtable_vals.txt");
        q = q_new;
        if (q.isTerminal() && ret == 0) {
            //ROS_INFO("(1)");
            //ROS_INFO("%d", q.eps_i);
            action = ACTION_E;
        } else {
            //ROS_INFO("(2)");
            //ROS_INFO("%d", q.eps_i);
            action = QT_->getAction(q);
        }

        bounded_eps = getBoundedEps(cur_eps);
        printStuff(timing_info, cur_eps, bounded_eps, reward);
        //ROS_INFO("bounded eps = %.3f", bounded_eps);
        ROS_INFO("-------------------");
        ROS_INFO("Action %d", action);
    }
    QT_->updateTerminal(q, action, reward);
    QT_->saveTable("/home/grogan/Qtable_vals.txt");

    // Write the start/goal sequence plus times for posterity
    FILE *fp;
    fp = fopen("/home/grogan/start_goal_sequence.txt", "a");
    fprintf(fp, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
            start_pose_.getX(), start_pose_.getY(), start_pose_.getTheta(),
            goal_pose_.getX(), goal_pose_.getY(), goal_pose_.getTheta(),
            timing_info.plan_time_, timing_info.exec_time_);
    fclose(fp);

    // done with planning by here
    
    //if (found_goal) {
    //    current_plan_ = retracePath(goal_state_);
    //} else if (current_best != NULL) {
    //    current_plan_ = retracePath(current_best);
    //}


    vel_path_pub_.publish(current_plan_);

    path = current_plan_.poses;

    ros::Time t = ros::Time::now();
    for (int i=0; i<path.size(); i++) {
        path[i].header.stamp = t;
        t += ros::Duration(time_resolution_);
    }

    if (!path.empty()) {
        return true;
    } else {
        return false;
    }
}

/********************************************************************
 * Get lower bound on cost of a solution for suboptimality bound (ARA*)
 * I absolutely hate the way I'm doing this. And it might be buggy
 ********************************************************************/
double LatticePlanner::getBoundedEps(double cur_eps) {
    //ROS_INFO("AStarLattice::getLowerBound");
    double lower_bound = DBL_MAX;
    State *state = NULL;
    //ROS_INFO("open_list size: %d", open_list_.size());
    for (int i=1; i<=open_list_->cur_size; i++) {
        state = open_list_->heap[i].heapState;
        double tmp = state->g + state->h;
        if (tmp < lower_bound) {
            lower_bound = tmp;
        }
    }

    state = incons_list_->getFirst();
    while (state != NULL) {  
        double tmp = state->g + state->h;
        if (tmp < lower_bound) {
            lower_bound = tmp;
        }
        state = incons_list_->getNext(state);
    }
    return std::min(cur_eps, goal_state_->g/lower_bound);
}

/********************************************************************
 * Move states from the inconsistent list to the open list and 
 * reevaluate their f values for the new heuristic inflation
 ********************************************************************/
void LatticePlanner::buildOpenList(double cur_eps) {
    //ROS_INFO("LatticePlanner::buildOpenList");
    // move from incons list
    while (incons_list_->firstElement != NULL) {
        State *state = incons_list_->firstElement->listState;
        double key = state->g + cur_eps*state->h;
        open_list_->insertElement(state, key);
        incons_list_->deleteElement(state);
    }
    // reevaluate keys
    for (int i=1; i<=open_list_->cur_size; i++) {
        State *state = open_list_->heap[i].heapState;
        open_list_->heap[i].key = state->g + cur_eps*state->h;
    }
    open_list_->makeHeap();
}

bool LatticePlanner::initStartAndGoal(geometry_msgs::PoseStamped start,
                                      geometry_msgs::PoseStamped goal) {
    // set up start and goal states
    start_pose_.setX(start.pose.position.x);
    start_pose_.setY(start.pose.position.y);
    start_pose_.setTheta(tf::getYaw(start.pose.orientation));
    goal_pose_.setX(goal.pose.position.x);
    goal_pose_.setY(goal.pose.position.y);
    goal_pose_.setTheta(tf::getYaw(goal.pose.orientation));

    // Allocate new start and goal states. 
    start_state_ = setStartState();
    goal_state_ = setGoalState();

    if (start_state_ == NULL || goal_state_ == NULL) {
        ROS_INFO("Bad start or goal state");
        return false;
    }
    if (start_state_->state_i.getDiagonalDistance(goal_state_->state_i) <= 5) {
        ROS_INFO("Start and goal state within tolerance");
        return true;
    }

    if (goal_state_->state_i.in_map == false) {
        ROS_INFO("Goal state not in map");
        ROS_INFO("goal->pose = (%f, %f, %f)", goal_state_->pose.getX(),
                                          goal_state_->pose.getY(),
                                          goal_state_->pose.getTheta());
        return false;
    }
    if (costmap_->getCostmap()->getCost(goal_state_->state_i.x_i, goal_state_->state_i.y_i)
            >= costmap_2d::INSCRIBED_INFLATED_OBSTACLE) {
        ROS_INFO("Goal state not reachable");
        return false;
    }

    // Set start state cost
    start_state_->g = 0.0;

    // insert start state into open list
    double key = start_state_->g + eps_*start_state_->h;
    open_list_->insertElement(start_state_, key);

    // save start/goal sequence for testing
    /*
    FILE *fp;
    fp = fopen("/home/grogan/start_goal_sequence.txt", "a");
    fprintf(fp, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
            start_pose_.getX(), start_pose_.getY(), start_pose_.getTheta(),
            goal_pose_.getX(), goal_pose_.getY(), goal_pose_.getTheta());
    fclose(fp);
    */
    
    return true;
}

/********************************************************************
 * Does what it says
 ********************************************************************/
State* LatticePlanner::setStartState() {
    State new_state;
    DiscreteState state_i = discretizer_->discretizeState(start_pose_, Velocity(0.0, 0.0));
    new_state.stateID = Hasher::getHash(new_state.state_i);
    new_state.state_i = state_i;
    new_state.pose = start_pose_;
    new_state.vel = Velocity(0.0, 0.0); // changes if replanning while moving
    new_state.parent = NULL;
    return getState(new_state);
}

/********************************************************************
 * Does what it says
 ********************************************************************/
State* LatticePlanner::setGoalState() {
    State new_state;
    DiscreteState state_i = discretizer_->discretizeState(goal_pose_, Velocity(0.0, 0.0));
    new_state.stateID = Hasher::getHash(new_state.state_i);
    new_state.state_i = state_i;
    new_state.pose = goal_pose_;
    new_state.vel = Velocity(0.0, 0.0); // changes if replanning while moving
    new_state.parent = NULL;
    return getState(new_state);
}

/********************************************************************
 * Retrace path from state using parent pointers. Return a Path
 ********************************************************************/
pseudo_lattice_planner::Path LatticePlanner::retracePath(State *state) {
    //ROS_INFO("LatticePlanner::retracePath");
    pseudo_lattice_planner::Path path;
    std::vector<geometry_msgs::PoseStamped> poses;
    std::vector<geometry_msgs::Twist> velocities;
    geometry_msgs::PoseStamped one_pose;
    geometry_msgs::Twist one_vel;
    while (state != NULL ) {
        std::string fixed_frame = costmap_->getGlobalFrameID();
        one_pose = state->pose.getStampedPose(fixed_frame, ros::Time::now());
        one_vel.linear.x = state->vel.vel_x;
        one_vel.angular.z = state->vel.vel_phi;
        poses.push_back(one_pose);
        velocities.push_back(one_vel);
        state = state->parent;
    }

    std::reverse(poses.begin(), poses.end());
    std::reverse(velocities.begin(), velocities.end());
      
    path.poses = poses;
    path.velocities = velocities;
    return path;
}

/********************************************************************
 * Publish the ole plannerooski
 ********************************************************************/
int LatticePlanner::publishPlan(State *state) {
    //ROS_INFO("LatticePlanner::publishPlan");
    current_plan_ = retracePath(state);
    std::vector<geometry_msgs::PoseStamped> plan_poses = current_plan_.poses;
    ros::Time t = ros::Time::now();
    for (int i=0; i<plan_poses.size(); i++) {
        plan_poses[i].header.frame_id = costmap_->getGlobalFrameID();
        plan_poses[i].header.stamp = t;
        t += ros::Duration(time_resolution_);
    }
    nav_msgs::Path path;
    if (!plan_poses.empty()) {
        path.header.stamp = plan_poses.at(0).header.stamp;
        path.header.frame_id = plan_poses.at(0).header.frame_id;
        path.poses = plan_poses;
        //ROS_INFO("publishing plan of size %d", plan_poses.size());
        path_pub_.publish(path);
    }
    return plan_poses.size();
}

void LatticePlanner::printStuff(TimingInfo ti, double cur_eps, double bounded_eps, double reward) {
    //ROS_INFO("-------------------------");
    ROS_INFO("goal_state_ cost %f", goal_state_->g);
    ROS_INFO("planning time %f", ti.plan_time_);
    ROS_INFO("estimated exec time %f", ti.exec_time_);
    ROS_INFO("reward %f", reward);
    //ROS_INFO("wall planning time %f", ti.plan_time_wall_);
    //ROS_INFO("wall improve time %f", ti.improve_time_wall_);
    ROS_INFO("current eps %f", cur_eps);
    ROS_INFO("bounded eps %f", bounded_eps);
    //ROS_INFO("eps_bound %f", eps_bound);
    //ROS_INFO("Start heuristic = %f/%f", 
    //        heuristic_calc_->getHeuristic(start_state_, goal_pose_),
    //        heuristic_calc_->max_val);
    //ROS_INFO("-------------------------");


    FILE *fp;
    fp = fopen("/home/grogan/planner_output.txt", "a");
    fprintf(fp, "%d, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.1f, %.3f, %.3f, %.3f, %.3f, %d, %d, %d\n",
            call_number_,
            ti.plan_time_,
            ti.improve_time_,
            reward,
            ti.plan_time_wall_,
            ti.improve_time_wall_,
            ti.exec_time_,
            cur_eps,
            bounded_eps,
            goal_state_->g,
            heuristic_calc_->getHeuristic(start_state_, goal_pose_),
            heuristic_calc_->max_val,
            open_list_->cur_size,
            incons_list_->cur_size,
            all_expanded_.size());
    fclose(fp);
} 


} /* namespace pseudo_lattice_planner */

