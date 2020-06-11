/*modified from the orginal Titan simulation libaray:https://github.com/jacobaustin123/Titan
ref: J. Austin, R. Corrales-Fatou, S. Wyetzner, and H. Lipson, �Titan: A Parallel Asynchronous Library for Multi-Agent and Soft-Body Robotics using NVIDIA CUDA,� ICRA 2020, May 2020.
*/

#ifndef TITAN_SIM_H
#define TITAN_SIM_H

#include "object.h"
#include "vec.h"
#include "shader.h"

#include <msgpack.hpp>

#ifdef GRAPHICS
#include "shader.h"

#include <GL/glew.h>// Include GLEW
#include <GLFW/glfw3.h>// Include GLFW
#include <glm/glm.hpp>// Include GLM
#include <glm/gtc/matrix_transform.hpp>
#include <cuda_gl_interop.h>
#endif

#include <thrust/host_vector.h>
#include <thrust/device_vector.h>

#include <algorithm>
#include <list>
#include <vector>
#include <set>
#include <thread>

#include <sstream>
#include <fstream>
#include<iostream>
#include<string.h>

constexpr int MAX_BLOCKS = 65535; // max number of CUDA blocks
constexpr int THREADS_PER_BLOCK = 64;
constexpr int MASS_THREADS_PER_BLOCK = 64;

constexpr int NUM_CUDA_STREAM = 5; // number of cuda stream excluding the default stream
constexpr int  NUM_QUEUED_KERNELS = 100; // number of kernels to queue at a given time (this will reduce the frequency of updates from the CPU by this factor

constexpr int NUM_UPDATE_PER_ROTATION = 5; //number of update per rotation

#define gpuErrchk(ans) { gpuAssert((ans), __FILE__, __LINE__); }
inline void gpuAssert(cudaError_t code, const char* file, int line, bool abort = true)
{
	if (code != cudaSuccess)
	{
		fprintf(stderr, "Cuda failure: %s %s %d\n", cudaGetErrorString(code), file, line);
		exit(code);
	}
}


struct StdJoint {
	std::vector<int> left;// the indices of the left points
	std::vector<int> right;// the indices of the right points
	std::vector<int> anchor;// the 2 indices of the anchor points: left_anchor_id,right_anchor_id
	int leftCoord;
	int rightCoord;
	MSGPACK_DEFINE(left, right, anchor, leftCoord, rightCoord);
};
class Model {
public:
	std::vector<std::vector<double> > vertices;// the mass xyzs
	std::vector<std::vector<int> > edges;//the spring ids
	std::vector<bool> isSurface;// whether the mass is near the surface
	std::vector<int> idVertices;// the edge id of the vertices
	std::vector<int> idEdges;// the edge id of the springs
	std::vector<std::vector<double> > colors;// the mass xyzs
	std::vector<StdJoint> Joints;// the joints
	MSGPACK_DEFINE(vertices, edges, isSurface, idVertices, idEdges, colors, Joints); // write the member variables that you want to pack
	Model() {}
	Model(const char* file_path) {
		// get the msgpack robot model
		// Deserialize the serialized data
		std::ifstream ifs(file_path, std::ifstream::in | std::ifstream::binary);
		std::stringstream buffer;
		buffer << ifs.rdbuf();
		msgpack::unpacked upd;//unpacked data
		msgpack::unpack(upd, buffer.str().data(), buffer.str().size());
		//    std::cout << upd.get() << std::endl;
		upd.get().convert(*this);
	}
};

struct MASS {
	double* m = nullptr;
	Vec3d* pos = nullptr;
	Vec3d* vel = nullptr;
	Vec3d* acc = nullptr;
	Vec3d* force = nullptr;
	Vec3d* force_extern = nullptr;
	Vec3d* color = nullptr;
	bool* fixed = nullptr;
	bool* constrain = nullptr;//whether to apply constrain on the mass, must be set true for constraint to work
	int num = 0;
#ifdef VERLET
	Vec3d* prev_pos = nullptr; // x_{n-1}, position at previous timestep
#endif // VERLET
	MASS() { }
	MASS(int num, bool on_host = true) {
		init(num, on_host);
	}
	void init(int num, bool on_host = true) {
		cudaError_t(*malloc)(void**, size_t);
		if (on_host) { malloc = &cudaMallocHost; }// if malloc = cudaMallocHost: allocate on host
		else { malloc = &cudaMalloc; }// if malloc = cudaMalloc: allocate on device		
		// ref: https://stackoverflow.com/questions/9410/how-do-you-pass-a-function-as-a-parameter-in-c
		// ref: https://www.cprogramming.com/tutorial/function-pointers.html
		gpuErrchk((*malloc)((void**)&m, num * sizeof(double)));
		gpuErrchk((*malloc)((void**)&pos, num * sizeof(Vec3d)));
		gpuErrchk((*malloc)((void**)&vel, num * sizeof(Vec3d)));
		gpuErrchk((*malloc)((void**)&acc, num * sizeof(Vec3d)));
		gpuErrchk((*malloc)((void**)&force, num * sizeof(Vec3d)));
		gpuErrchk((*malloc)((void**)&force_extern, num * sizeof(Vec3d)));
		gpuErrchk((*malloc)((void**)&color, num * sizeof(Vec3d)));
		gpuErrchk((*malloc)((void**)&fixed, num * sizeof(bool)));
		gpuErrchk((*malloc)((void**)&constrain, num * sizeof(bool)));
		this->num = num;
#ifdef VERLET
		gpuErrchk((*malloc)((void**)&prev_pos, num * sizeof(Vec3d)));
#endif // VERLET
		if (on_host) {// set vel,acc to 0
			memset(vel, 0, num * sizeof(Vec3d));
			memset(acc, 0, num * sizeof(Vec3d));
#ifdef VERLET
			memset(prev_pos,0,num * sizeof(Vec3d));
#endif // VERLET
		}
		else {
			cudaMemset(vel, 0, num * sizeof(Vec3d));
			cudaMemset(acc, 0, num * sizeof(Vec3d));
		}
	}
	void copyFrom(MASS& other, cudaStream_t stream=(cudaStream_t)0) {
		gpuErrchk(cudaMemcpyAsync(m, other.m, num * sizeof(double), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(pos, other.pos, num * sizeof(Vec3d), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(vel, other.vel, num * sizeof(Vec3d), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(acc, other.acc, num * sizeof(Vec3d), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(force, other.force, num * sizeof(Vec3d), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(force_extern, other.force_extern, num * sizeof(Vec3d), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(color, other.color, num * sizeof(Vec3d), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(fixed, other.fixed, num * sizeof(bool), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(constrain, other.constrain, num * sizeof(bool), cudaMemcpyDefault, stream));
#ifdef VERLET
		gpuErrchk(cudaMemcpyAsync(prev_pos, other.force_extern, num * sizeof(Vec3d), cudaMemcpyDefault, stream));
#endif // VERLET
		//this->num = other.num;
	}
	void CopyPosVelAccFrom(MASS& other, cudaStream_t stream = (cudaStream_t)0) {
		gpuErrchk(cudaMemcpyAsync(pos, other.pos, num * sizeof(Vec3d), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(vel, other.vel, num * sizeof(Vec3d), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(acc, other.acc, num * sizeof(Vec3d), cudaMemcpyDefault, stream));
	}
};

struct SPRING {
	double* k = nullptr; // spring constant (N/m)
	double* rest = nullptr; // spring rest length (meters)
	double* damping = nullptr; // damping on the masses.
	Vec2i* edge = nullptr;// (left,right) mass indices of the spring
	bool* resetable = nullptr; // a flag indicating whether to reset every dynamic update
	int num =0;
	SPRING() {}
	SPRING(int num, bool on_host = true) {
		init(num, on_host);
	}
	void init(int num, bool on_host = true) { // initialize
		cudaError_t(*malloc)(void**, size_t);
		if (on_host) { malloc = &cudaMallocHost; }// if malloc = cudaMallocHost: allocate on host
		else { malloc = &cudaMalloc; }// if malloc = cudaMalloc: allocate on device		
		// ref: https://www.cprogramming.com/tutorial/function-pointers.html
		gpuErrchk((*malloc)((void**)&k, num * sizeof(double)));
		gpuErrchk((*malloc)((void**)&rest, num * sizeof(double)));
		gpuErrchk((*malloc)((void**)&damping, num * sizeof(double)));
		gpuErrchk((*malloc)((void**)&edge, num * sizeof(Vec2i)));
		gpuErrchk((*malloc)((void**)&resetable, num * sizeof(bool)));
		this->num = num;
	}
	void copyFrom(const SPRING& other, cudaStream_t stream = (cudaStream_t)0) { // assuming we have enough streams
		gpuErrchk(cudaMemcpyAsync(k, other.k, num * sizeof(double), cudaMemcpyDefault,stream));
		gpuErrchk(cudaMemcpyAsync(rest, other.rest, num * sizeof(double), cudaMemcpyDefault,stream));
		gpuErrchk(cudaMemcpyAsync(damping, other.damping, num * sizeof(double), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(edge, other.edge, num * sizeof(Vec2i), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(resetable, other.resetable, num * sizeof(bool), cudaMemcpyDefault, stream));
		//this->num = other.num;
	}
};


struct RotAnchors { // the anchors that belongs to the rotational joints
	Vec2i* edge; // index of the (left,right) anchor of the joint
	Vec3d* dir; // direction of the joint,normalized
	double* theta;// the angular increment per joint update

	int* leftCoord; // the index of left coordintate (oxyz) start for all joints (flat view)
	int* rightCoord;// the index of right coordintate (oxyz) start for all joints (flat view)

	int num; // num of anchor
	RotAnchors(){}
	RotAnchors(std::vector<StdJoint> std_joints, bool on_host = true) {init(std_joints, on_host);}

	void init(std::vector<StdJoint> std_joints, bool on_host = true) {
		num = std_joints.size();

		cudaError_t(*malloc)(void**, size_t);
		if (on_host) { malloc = &cudaMallocHost; }// if malloc = cudaMallocHost: allocate on host
		else { malloc = &cudaMalloc; }// if malloc = cudaMalloc: allocate on device	

		(*malloc)((void**)&edge, num * sizeof(Vec2i));
		(*malloc)((void**)&dir, num * sizeof(Vec3d));
		(*malloc)((void**)&theta, num * sizeof(double));
		(*malloc)((void**)&leftCoord, num * sizeof(int));
		(*malloc)((void**)&rightCoord, num * sizeof(int));
		gpuErrchk(cudaPeekAtLastError());
		if (on_host) { // copy the std_joints to this
			for (int joint_id = 0; joint_id < num; joint_id++)
			{
				edge[joint_id] = std_joints[joint_id].anchor;
				leftCoord[joint_id] = std_joints[joint_id].leftCoord;
				rightCoord[joint_id] = std_joints[joint_id].rightCoord;
			}
		}
	}

	void copyFrom(const RotAnchors& other, cudaStream_t stream = (cudaStream_t)0) {
		cudaMemcpyAsync(edge, other.edge, num * sizeof(Vec2i), cudaMemcpyDefault, stream);
		cudaMemcpyAsync(dir, other.dir, num * sizeof(Vec3d), cudaMemcpyDefault, stream);
		cudaMemcpyAsync(theta, other.theta, num * sizeof(double), cudaMemcpyDefault, stream);
		cudaMemcpyAsync(leftCoord, other.leftCoord, num * sizeof(int), cudaMemcpyDefault, stream);
		cudaMemcpyAsync(rightCoord, other.rightCoord, num * sizeof(int), cudaMemcpyDefault, stream);
		gpuErrchk(cudaPeekAtLastError());

	}
	void copyThetaFrom(const RotAnchors& other, cudaStream_t stream = (cudaStream_t)0) {
		gpuErrchk(cudaMemcpyAsync(theta, other.theta, num * sizeof(double), cudaMemcpyDefault, stream));
	}
};

struct RotPoints { // the points that belongs to the rotational joints
	int* massId; // index of the left mass and right mass
	// the directional anchor index of which the mass is rotated about, 
	int* anchorId;// e.g, k: the k-th anchor,left mass, -k: the k-th anchor, right mass
	int* dir; // direction: left=-1,right=+1
	int num; // the length of array "id"

	RotPoints(){}
	RotPoints(std::vector<StdJoint> std_joints, bool on_host = true) { init(std_joints, on_host); }
	void init(std::vector<StdJoint> std_joints, bool on_host = true) {
		num = 0;
		for each (auto & std_joint in std_joints)
		{num += std_joint.left.size() + std_joint.right.size();}// get the total number of the points in all joints

		// allocate on host or device
		cudaError_t(*malloc)(void**, size_t);
		if (on_host) { malloc = &cudaMallocHost; }// if malloc = cudaMallocHost: allocate on host
		else { malloc = &cudaMalloc; }// if malloc = cudaMalloc: allocate on device		
		gpuErrchk((*malloc)((void**)&massId, num * sizeof(int)));
		gpuErrchk((*malloc)((void**)&anchorId, num * sizeof(int)));
		gpuErrchk((*malloc)((void**)&dir, num * sizeof(int)));


		if (on_host) { // copy the std_joints to this
			int offset = 0;//offset the index by "offset"
			for (int joint_id = 0; joint_id < std_joints.size(); joint_id++)
			{
				StdJoint& std_joint = std_joints[joint_id];

				for (int i = 0; i < std_joint.left.size(); i++)
				{
					massId[offset + i] = std_joint.left[i];
					anchorId[offset + i] = joint_id;
					dir[offset + i] = -1;
				}
				offset += std_joint.left.size();//increment offset by num of left

				for (int i = 0; i < std_joint.right.size(); i++)
				{
					massId[offset + i] = std_joint.right[i];
					anchorId[offset + i] = joint_id;
					dir[offset + i] = 1;
				}
				offset += std_joint.right.size();//increment offset by num of right
			}
		}
	}
	void copyFrom(const RotPoints& other, cudaStream_t stream = (cudaStream_t)0) {
		gpuErrchk(cudaMemcpyAsync(massId, other.massId, num * sizeof(int), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(anchorId, other.anchorId, num * sizeof(int), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(dir, other.dir, num * sizeof(int), cudaMemcpyDefault, stream));
	}
};

struct JOINT {
	RotPoints points;
	RotAnchors anchors;

	JOINT() {};
	JOINT(std::vector<StdJoint> std_joints, bool on_host = true) {init(std_joints, on_host);};
	void copyFrom(const JOINT& other, cudaStream_t stream = (cudaStream_t)0) {
		points.copyFrom(other.points, stream); // copy from the other points
		anchors.copyFrom(other.anchors, stream); // copy from the other anchor
	}

	void init(std::vector<StdJoint> std_joints, bool on_host = true) {
		anchors.init(std_joints, on_host);//initialize anchor
		points.init(std_joints, on_host);
	}
	inline int size() { return anchors.num; }
};

class Simulation {
public:
	double dt = 0.0001;
	double T = 0; //simulation time
	Vec3d global_acc = Vec3d(0,0,0); // global acceleration

	int id_restable_spring_start = 0; // resetable springs start index (inclusive)
	int id_resetable_spring_end = 0; // resetable springs start index (exclusive)

	int id_oxyz_start = 0;// coordinate oxyz start index (inclusive)
	int id_oxyz_end = 0; // coordinate oxyz end index (exclusive)

	// host
	MASS mass; // a flat fiew of all masses
	SPRING spring; // a flat fiew of all springs
	JOINT joints;// a flat view of all joints
	// device
	MASS d_mass;
	SPRING d_spring;
	JOINT d_joints;
	
	double* joint_angles; // (measured) joint angle array in rad, initialized in start()
	double* joint_speeds; // (measured) joint speed array in rad/s, initialized in start()
	double* joint_speeds_cmd; // (commended) joint speed array in RPM, initialized in start()
	double max_joint_speed = 1e-4; // 


	//size_t num_mass=0;// refer to mass.num
	//size_t num_spring=0;//refer to spring.num
	//int num_joint = 4; //refer to joints.size()


	// control
	bool RUNNING = false;
	bool STARTED = false;
	bool ENDED = false;

	bool FREED = false;
	bool GPU_DONE = false;

	cudaStream_t stream[NUM_CUDA_STREAM]; // cuda stream:https://docs.nvidia.com/cuda/cuda-c-programming-guide/index.html#asynchronous-concurrent-execution

	Simulation();
	Simulation(int num_mass, int num_spring);
	~Simulation();

	void getAll();
	void setAll();
	void setMass();
	// Global constraints (can be rendered)
	// creates half-space ax + by + cz < d
	void createPlane(const Vec3d& abc, const double d, const double FRICTION_K = 0, const double FRICTION_S = 0);
	void createBall(const Vec3d& center, const double r); // creates ball with radius r at position center
	void clearConstraints(); // clears global constraints only

	void setBreakpoint(const double time); // tell the program to stop at a fixed time (doesn't hang).

	void start();

	void pause(const double t);//pause the simulation at (simulation) time t [s]


	void resume();

	void _run();
	void execute(); // same as above but w/out reset

#ifdef DEBUG_ENERGY
	double energy(); //compute the total energy of the system
	double energy_start;
	double energy_deviation_max = 0;
#endif // DEBUG_ENERGY

#ifdef GRAPHICS
	void setViewport(const Vec3d& camera_position, const Vec3d& target_location, const Vec3d& up_vector);
	void moveViewport(const Vec3d& displacement);
#endif

private:

	void waitForEvent();
	void freeGPU();
	inline void updateCudaParameters();
	inline int computeBlocksPerGrid(const int threadsPerBlock, const int num);//helper function to compute blocksPerGrid

	std::thread gpu_thread;
	std::set<double> bpts; // list of breakpoints

	int massBlocksPerGrid; // blocksPergrid for mass update
	int springBlocksPerGrid; // blocksPergrid for spring update
	int jointBlocksPerGrid;// blocksPergrid for joint rotation

	std::vector<Constraint*> constraints;
	thrust::device_vector<CudaContactPlane> d_planes; // used for constraints
	thrust::device_vector<CudaBall> d_balls; // used for constraints

	CUDA_GLOBAL_CONSTRAINTS d_constraints;
	bool update_constraints = true;


#ifdef GRAPHICS
	int line_width = 3; // line width for rendering the springs
	int point_size = 3; // point size for rendering the masses

	GLFWwindow* window;
	int window_width,window_height; // the width and height of the window

	GLuint VertexArrayID; // handle for the vertex array object
	GLuint programID;  // handle for the shader program
	GLuint MatrixID; // handel for the uniform MVP

	glm::mat4 MVP; //model-view-projection matrix
	glm::mat4 View; //view matrix
	glm::mat4 Projection; //projection matrix

	// for projection matrix 
	Vec3d camera_pos;// camera position
	Vec3d camera_dir;//camera look at direction
	//Vec3d looks_at;
	Vec3d camera_up;// camera up

	GLuint vertexbuffer; // handle for vertexbuffer
	GLuint colorbuffer; // handle for colorbuffer
	GLuint elementbuffer; // handle for elementbuffer

	void* vertexPointer;// used in updateBuffers(), stores positions of the vertices
	void* indexPointer; // used in updateBuffers(), stores indices (line plot)
	void* colorPointer; // used in updateBuffers(), stores colors of the vertices


	bool update_indices = true; // update vertexbuffer if true
	bool update_colors = true; // update colorbuffer if true
	bool resize_buffers = true; // update all (vertexbuffer,colorbuffer,elementbuffer) if true

	void computeMVP(bool update_view = true); // compute MVP

	inline void updateBuffers();
	inline void updateVertexBuffers();//only update vertex (positions)
	inline void generateBuffers();
	inline void resizeBuffers();
	inline void draw();

	void createGLFWWindow();

#endif
};


#ifdef GRAPHICS
void framebuffer_size_callback(GLFWwindow* window, int width, int height);

#endif


#endif //TITAN_SIM_H