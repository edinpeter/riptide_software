#include "ros/ros.h"
#include "geometry_msgs/Accel.h"
#include "geometry_msgs/Vector3.h"
#include "riptide_msgs/ThrustStamped.h"
#include "tf/transform_listener.h"

#include <math.h>
#include <vector>
#include "ceres/ceres.h"
#include "glog/logging.h"

#undef debug
#undef report
#undef progress

// Thrust limits (N):
double MIN_THRUST = -18.0;
double MAX_THRUST = 18.0;

// Vehicle mass (kg):
double MASS = 48.8428;

// Moments of inertia (kg*m^2)
double Ix = 0.55649783;
double Iy = 1.89075467;
double Iz = 1.96057706;

// Acceleration commands (m/s^):
double cmdSurge = 0.0;
double cmdSway = 0.0;
double cmdHeave = 0.0;
double cmdRoll = 0.0;
double cmdPitch = 0.0;
double cmdYaw = 0.0;

struct vector
{
		double x;
		double y;
		double z;
};

vector makeVector(double X, double Y, double Z)
{
	vector v;
	v.x = X;
	v.y = Y;
	v.z = Z;
	return v;
}

/*** Thruster Positions ***/
// Positions are in meters relative to the center of mass.
vector pos_surge_stbd_hi;
vector pos_surge_port_hi;
vector pos_surge_port_lo;
vector pos_surge_stbd_lo;
vector pos_sway_fwd;
vector pos_sway_aft;
vector pos_heave_port_aft;
vector pos_heave_stbd_aft;
vector pos_heave_stbd_fwd;
vector pos_heave_port_fwd;

/*** EQUATIONS ***/
// These equations solve for linear/angular acceleration in all axes

// Linear Equations
struct surge {
	template <typename T> bool operator()(const T* const surge_stbd_hi,
																				const T* const surge_port_hi,
																				const T* const surge_port_lo,
																				const T* const surge_stbd_lo,
																				T* residual) const
	{
		residual[0] = (surge_stbd_hi[0] + surge_port_hi[0] + surge_port_lo[0] + surge_stbd_lo[0]) / T(MASS) - T(cmdSurge);
		return true;
	}
};

struct sway {
	template <typename T> bool operator()(const T* const sway_fwd,
																				const T* const sway_aft,
																				T* residual) const
	{
		residual[0] = (sway_fwd[0] + sway_aft[0]) / T(MASS) - T(cmdSway);
		return true;
	}
};

struct heave {
	template <typename T> bool operator()(const T* const heave_port_aft,
																				const T* const heave_stbd_aft,
																				const T* const heave_stbd_fwd,
																				const T* const heave_port_fwd,
																				T* residual) const
	{
		residual[0] = (heave_port_aft[0] + heave_stbd_aft[0] + heave_stbd_fwd[0] + heave_port_fwd[0]) / T(MASS) - T(cmdHeave);
		return true;
	}
};

// Angular equations
struct roll {
	template <typename T> bool operator()(const T* const heave_port_aft,
																				const T* const heave_stbd_aft,
																				const T* const heave_stbd_fwd,
																				const T* const heave_port_fwd,
																				const T* const sway_fwd,
																				const T* const sway_aft,
																				T* residual) const
	{
		residual[0] = (heave_port_aft[0]*T(pos_heave_port_aft.y)+heave_stbd_aft[0]*T(pos_heave_stbd_aft.y)+heave_stbd_fwd[0]*T(pos_heave_stbd_fwd.y)+heave_port_fwd[0]*T(pos_heave_port_fwd.y)+sway_fwd[0]*T(pos_sway_fwd.z)+sway_aft[0]*T(pos_sway_aft.z)) / T(Ix) - T(cmdRoll);
		return true;
	}
};

struct pitch {
	template <typename T> bool operator()(const T* const surge_stbd_hi,
																				const T* const surge_port_hi,
																				const T* const surge_port_lo,
																				const T* const surge_stbd_lo,
																				const T* const heave_port_aft,
																				const T* const heave_stbd_aft,
																				const T* const heave_stbd_fwd,
																				const T* const heave_port_fwd,
																				T* residual) const
	{
		residual[0] = (surge_stbd_hi[0] * T(pos_surge_stbd_hi.z) + surge_port_hi[0]*T(pos_surge_port_hi.z) + surge_port_lo[0]*T(pos_surge_port_lo.z) + surge_stbd_lo[0]*T(pos_surge_stbd_lo.z) + heave_port_aft[0]*T(pos_heave_port_aft.x) + heave_stbd_aft[0]*T(pos_heave_stbd_aft.x) + heave_stbd_fwd[0]*T(pos_heave_stbd_fwd.x) + heave_port_fwd[0]*T(pos_heave_port_fwd.x)) / T(Iy) - T(cmdPitch);
		return true;
	}
};

struct yaw{
	template <typename T> bool operator()(const T* const surge_stbd_hi,
																								 const T* const surge_port_hi,
																			 					 const T* const surge_port_lo,
																			 					 const T* const surge_stbd_lo,
																			 					 const T* const sway_fwd,
																			 					 const T* const sway_aft,
																			 					 T* residual) const
	{
		residual[0] = (surge_stbd_hi[0]*T(pos_surge_stbd_hi.y) + surge_port_hi[0]*T(pos_surge_port_hi.y) + surge_port_lo[0]*T(pos_surge_port_lo.y) + surge_stbd_lo[0]*T(pos_surge_stbd_lo.y) + sway_fwd[0]*T(pos_sway_fwd.x) + sway_aft[0]*T(pos_sway_aft.x)) / T(Iz) - T(cmdYaw);
		return true;
	}
};

class Solver
{
	private:
		// Comms
		ros::NodeHandle nh;
		ros::Subscriber cmd_sub;
		ros::Publisher cmd_pub;
		riptide_msgs::ThrustStamped thrust;
		// Math
		ceres::Problem problem;
		ceres::Solver::Options options;
		ceres::Solver::Summary summary;
		// Results
		double surge_stbd_hi, surge_port_hi, surge_port_lo, surge_stbd_lo;
		double sway_fwd, sway_aft;
		double heave_port_aft, heave_stbd_aft, heave_stbd_fwd, heave_port_fwd;
		// TF
		tf::TransformListener *listener;
		tf::StampedTransform surge_1;
		tf::StampedTransform surge_2;
		tf::StampedTransform surge_3;
		tf::StampedTransform surge_4;
		tf::StampedTransform sway_1;
		tf::StampedTransform sway_2;
		tf::StampedTransform heave_1;
		tf::StampedTransform heave_2;
		tf::StampedTransform heave_3;
		tf::StampedTransform heave_4;

	public:
		Solver(char** argv, tf::TransformListener& listener_adr);
		void callback(const geometry_msgs::Accel::ConstPtr& a);
		void loop();
};

int main(int argc, char **argv)
{
	ros::init(argc, argv, "thrust_solver");
  tf::TransformListener tf_listener;
	Solver solver(argv, tf_listener);
	solver.loop();
}

Solver::Solver(char** argv, tf::TransformListener& listener_adr)
{
	listener = &listener_adr;

	listener->waitForTransform("/base_link", "/surge_port_hi_thruster", ros::Time(0), ros::Duration(10.0) );
	listener->lookupTransform("/base_link", "/surge_port_hi_thruster", ros::Time(0), surge_2);
	listener->waitForTransform("/base_link", "/surge_stbd_hi_thruster", ros::Time(0), ros::Duration(10.0) );
	listener->lookupTransform("/base_link", "/surge_stbd_hi_thruster", ros::Time(0), surge_1);
  listener->waitForTransform("/base_link", "/surge_port_lo_thruster", ros::Time(0), ros::Duration(10.0) );
	listener->lookupTransform("/base_link", "/surge_port_lo_thruster", ros::Time(0), surge_4);
  listener->waitForTransform("/base_link", "/surge_stbd_lo_thruster", ros::Time(0), ros::Duration(10.0) );
	listener->lookupTransform("/base_link", "/surge_stbd_lo_thruster", ros::Time(0), surge_3);
	listener->waitForTransform("/base_link", "/sway_fwd_thruster", ros::Time(0), ros::Duration(10.0) );
	listener->lookupTransform("/base_link", "/sway_fwd_thruster", ros::Time(0), sway_1);
	listener->waitForTransform("/base_link", "/sway_aft_thruster", ros::Time(0), ros::Duration(10.0) );
	listener->lookupTransform("/base_link", "/sway_aft_thruster", ros::Time(0), sway_2);
	listener->waitForTransform("/base_link", "/heave_port_fwd_thruster", ros::Time(0), ros::Duration(10.0) );
	listener->lookupTransform("/base_link", "/heave_port_fwd_thruster", ros::Time(0), heave_2);
	listener->waitForTransform("/base_link", "/heave_stbd_fwd_thruster", ros::Time(0), ros::Duration(10.0) );
	listener->lookupTransform("/base_link", "/heave_stbd_fwd_thruster", ros::Time(0), heave_1);
	listener->waitForTransform("/base_link", "/heave_port_aft_thruster", ros::Time(0), ros::Duration(10.0) );
	listener->lookupTransform("/base_link", "/heave_port_aft_thruster", ros::Time(0), heave_4);
	listener->waitForTransform("/base_link", "/heave_stbd_aft_thruster", ros::Time(0), ros::Duration(10.0) );
	listener->lookupTransform("/base_link", "/heave_stbd_aft_thruster", ros::Time(0), heave_3);

	pos_surge_port_hi = makeVector(surge_2.getOrigin().x(), surge_2.getOrigin().y(), surge_2.getOrigin().z());
	pos_surge_stbd_hi = makeVector(surge_1.getOrigin().x(), surge_1.getOrigin().y(), surge_1.getOrigin().z());
	pos_surge_port_lo = makeVector(surge_3.getOrigin().x(), surge_3.getOrigin().y(), surge_3.getOrigin().z());
	pos_surge_stbd_lo = makeVector(surge_4.getOrigin().x(), surge_4.getOrigin().y(), surge_4.getOrigin().z());
	pos_sway_fwd = makeVector(sway_1.getOrigin().x(), sway_1.getOrigin().y(), sway_1.getOrigin().z());
	pos_sway_aft = makeVector(sway_2.getOrigin().x(), sway_2.getOrigin().y(), sway_2.getOrigin().z());
	pos_heave_port_fwd = makeVector(heave_4.getOrigin().x(), heave_4.getOrigin().y(), heave_4.getOrigin().z());
	pos_heave_stbd_fwd = makeVector(heave_3.getOrigin().x(), heave_3.getOrigin().y(), heave_3.getOrigin().z());
	pos_heave_port_aft = makeVector(heave_1.getOrigin().x(), heave_1.getOrigin().y(), heave_1.getOrigin().z());
	pos_heave_stbd_aft = makeVector(heave_2.getOrigin().x(), heave_2.getOrigin().y(), heave_2.getOrigin().z());

	cmd_sub = nh.subscribe<geometry_msgs::Accel>("command/accel", 1, &Solver::callback, this);
	cmd_pub = nh.advertise<riptide_msgs::ThrustStamped>("command/thrust", 1); // Message containing force required from each thruster to achieve given acceleration

	google::InitGoogleLogging(argv[0]);

	// PROBLEM SETUP

	// Add residual blocks (equations)

	// Linear
	problem.AddResidualBlock(new ceres::AutoDiffCostFunction<surge, 1, 1, 1, 1, 1>(new surge),
		NULL,
		&surge_stbd_hi, &surge_port_hi, &surge_port_lo, &surge_stbd_lo);
	problem.AddResidualBlock(new ceres::AutoDiffCostFunction<sway, 1, 1, 1>(new sway),
		NULL,
		&sway_fwd, &sway_aft);
	problem.AddResidualBlock(new ceres::AutoDiffCostFunction<heave, 1, 1, 1, 1, 1>(new heave),
		NULL,
		&heave_port_aft, &heave_stbd_aft, &heave_stbd_fwd, &heave_port_fwd);

	// Angular
	problem.AddResidualBlock(new ceres::AutoDiffCostFunction<roll, 1, 1, 1, 1, 1, 1, 1>(new roll),
		NULL,
		&heave_port_aft, &heave_stbd_aft, &heave_stbd_fwd, &heave_port_fwd, &sway_fwd, &sway_aft);
	problem.AddResidualBlock(new ceres::AutoDiffCostFunction<pitch, 1, 1, 1, 1, 1, 1, 1, 1, 1>(new pitch),
		NULL,
		&surge_stbd_hi, &surge_port_hi, &surge_port_lo, &surge_stbd_lo, &heave_port_aft, &heave_stbd_aft, &heave_stbd_fwd, &heave_port_fwd);
	problem.AddResidualBlock(new ceres::AutoDiffCostFunction<yaw, 1, 1, 1, 1, 1, 1, 1>(new yaw),
		NULL,
		&surge_stbd_hi, &surge_port_hi, &surge_port_lo, &surge_stbd_lo, &sway_fwd, &sway_aft);

	// Set constraints (min/max thruster force)

	// Surge thrusters
	problem.SetParameterLowerBound(&surge_port_hi, 0, MIN_THRUST);
	problem.SetParameterUpperBound(&surge_port_hi, 0, MAX_THRUST);

	problem.SetParameterLowerBound(&surge_stbd_hi, 0, MIN_THRUST);
	problem.SetParameterUpperBound(&surge_stbd_hi, 0, MAX_THRUST);

	problem.SetParameterLowerBound(&surge_port_lo, 0, MIN_THRUST);
	problem.SetParameterUpperBound(&surge_port_lo, 0, MAX_THRUST);

	problem.SetParameterLowerBound(&surge_stbd_lo, 0, MIN_THRUST);
	problem.SetParameterUpperBound(&surge_stbd_lo, 0, MAX_THRUST);

	// Sway thrusters
	problem.SetParameterLowerBound(&sway_fwd, 0, MIN_THRUST);
	problem.SetParameterUpperBound(&sway_fwd, 0, MAX_THRUST);

	problem.SetParameterLowerBound(&sway_aft, 0, MIN_THRUST);
	problem.SetParameterUpperBound(&sway_aft, 0, MAX_THRUST);

	// Heave thrusters
	problem.SetParameterLowerBound(&heave_port_fwd, 0, MIN_THRUST);
	problem.SetParameterUpperBound(&heave_port_fwd, 0, MAX_THRUST);

	problem.SetParameterLowerBound(&heave_stbd_fwd, 0, MIN_THRUST);
	problem.SetParameterUpperBound(&heave_stbd_fwd, 0, MAX_THRUST);

	problem.SetParameterLowerBound(&heave_port_aft, 0, MIN_THRUST);
	problem.SetParameterUpperBound(&heave_port_aft, 0, MAX_THRUST);

	problem.SetParameterLowerBound(&heave_stbd_aft, 0, MIN_THRUST);
	problem.SetParameterUpperBound(&heave_stbd_aft, 0, MAX_THRUST);

	// Configure solver
	options.max_num_iterations = 100;
	options.linear_solver_type = ceres::DENSE_QR;

#ifdef progress
	options.minimizer_progress_to_stdout = true;
#endif
}

void Solver::callback(const geometry_msgs::Accel::ConstPtr& a)
{
	cmdSurge = a->linear.x;
	cmdSway = a->linear.y;
	cmdHeave = a->linear.z;

	cmdRoll = a->angular.x;
	cmdPitch = a->angular.y;
	cmdYaw = a->angular.z;

	// These forced initial guesses don't make much of a difference.
	// We currently experience a sort of gimbal lock w/ or w/o them.
	surge_stbd_hi = 0.0;
	surge_port_hi = 0.0;
	surge_port_lo = 0.0;
	surge_stbd_lo = 0.0;
	sway_fwd = 0.0;
	sway_aft = 0.0;
	heave_port_aft = 0.0;
	heave_stbd_aft = 0.0;
	heave_stbd_fwd = 0.0;
	heave_port_fwd = 0.0;

#ifdef debug
	std::cout << "Initial surge_stbd_hi = " << surge_stbd_hi
						<< ", surge_port_hi = " << surge_port_hi
						<< ", surge_port_lo = " << surge_port_lo
						<< ", surge_stbd_lo = " << surge_stbd_lo
						<< ", sway_fwd = " << sway_fwd
						<< ", sway_aft = " << sway_aft
						<< ", heave_port_aft = " << heave_port_aft
						<< ", heave_stbd_aft = " << heave_stbd_aft
						<< ", heave_stbd_fwd = " << heave_stbd_fwd
						<< ", heave_port_fwd = " << heave_port_fwd
						<< std::endl;
#endif

	// Solve all my problems
	ceres::Solve(options, &problem, &summary);

#ifdef report
	std::cout << summary.FullReport() << std::endl;
#endif

#ifdef debug
	std::cout << "Final surge_stbd_hi = " << surge_stbd_hi
						<< ", surge_port_hi = " << surge_port_hi
						<< ", surge_port_lo = " << surge_port_lo
						<< ", surge_stbd_lo = " << surge_stbd_lo
						<< ", sway_fwd = " << sway_fwd
						<< ", sway_aft = " << sway_aft
						<< ", heave_port_aft = " << heave_port_aft
						<< ", heave_stbd_aft = " << heave_stbd_aft
						<< ", heave_stbd_fwd = " << heave_stbd_fwd
						<< ", heave_port_fwd = " << heave_port_fwd
						<< std::endl;
#endif

	// Create stamped thrust message
	thrust.header.stamp = ros::Time::now();;

	thrust.force.surge_stbd_hi = surge_stbd_hi;
	thrust.force.surge_port_hi = surge_port_hi;
	thrust.force.surge_port_lo = surge_port_lo;
	thrust.force.surge_stbd_lo = surge_stbd_lo;
	thrust.force.sway_fwd = sway_fwd;
	thrust.force.sway_aft = sway_aft;
	thrust.force.heave_port_aft = heave_port_aft;
	thrust.force.heave_stbd_aft = heave_stbd_aft;
	thrust.force.heave_stbd_fwd = heave_stbd_fwd;
	thrust.force.heave_port_fwd = heave_port_fwd;

	cmd_pub.publish(thrust);
}

void Solver::loop()
{
	ros::spin();
}
