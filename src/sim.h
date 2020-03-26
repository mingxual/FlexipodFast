#ifndef TITAN_SIM_H
#define TITAN_SIM_H


#include "object.h"
#include "vec.h"
#include "shader.h"


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

constexpr int MAX_BLOCKS = 65535; // max number of CUDA blocks
constexpr int THREADS_PER_BLOCK = 128;
constexpr int MASS_THREADS_PER_BLOCK = 32;

constexpr int NUM_CUDA_STREAM = 5; // number of cuda stream besides the default stream
constexpr int  NUM_QUEUED_KERNELS = 16; // number of kernels to queue at a given time (this will reduce the frequency of updates from the CPU by this factor

#define gpuErrchk(ans) { gpuAssert((ans), __FILE__, __LINE__); }
inline void gpuAssert(cudaError_t code, const char* file, int line, bool abort = false)
{
	if (code != cudaSuccess)
	{
		fprintf(stderr, "GPUassert: %s %s %d\n", cudaGetErrorString(code), file, line);
		if (abort) {
			char buffer[200];
			snprintf(buffer, sizeof(buffer), "GPUassert error in CUDA kernel: %s %s %d\n", cudaGetErrorString(code), file, line);
			std::string buffer_string = buffer;
			throw std::runtime_error(buffer_string);
		}
	}
}


struct MASS {
	double* m;
	Vec* pos;
	Vec* vel;
	Vec* acc;
	Vec* force;
	Vec* force_extern;
	Vec* color;
	bool* fixed;
	int num;
	MASS() { num = 0; }
	MASS(int num, bool on_host = true) {
		cudaError_t(*malloc)(void**, size_t);
		if (on_host) { malloc = &cudaMallocHost; }// if malloc = cudaMallocHost: allocate on host
		else { malloc = &cudaMalloc; }// if malloc = cudaMalloc: allocate on device		
		// ref: https://stackoverflow.com/questions/9410/how-do-you-pass-a-function-as-a-parameter-in-c
		// ref: https://www.cprogramming.com/tutorial/function-pointers.html
		gpuErrchk((*malloc)((void**)&m, num * sizeof(double)));
		gpuErrchk((*malloc)((void**)&pos, num * sizeof(Vec)));
		gpuErrchk((*malloc)((void**)&vel, num * sizeof(Vec)));
		gpuErrchk((*malloc)((void**)&acc, num * sizeof(Vec)));
		gpuErrchk((*malloc)((void**)&force, num * sizeof(Vec)));
		gpuErrchk((*malloc)((void**)&force_extern, num * sizeof(Vec)));
		gpuErrchk((*malloc)((void**)&color, num * sizeof(Vec)));
		gpuErrchk((*malloc)((void**)&fixed, num * sizeof(bool)));
		this->num = num;
	}
	void copyFrom(MASS& other, cudaStream_t stream=(cudaStream_t)0) {
		gpuErrchk(cudaMemcpyAsync(m, other.m, num * sizeof(double), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(pos, other.pos, num * sizeof(Vec), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(vel, other.vel, num * sizeof(Vec), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(acc, other.acc, num * sizeof(Vec), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(force, other.force, num * sizeof(Vec), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(force_extern, other.force_extern, num * sizeof(Vec), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(color, other.color, num * sizeof(Vec), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(fixed, other.fixed, num * sizeof(bool), cudaMemcpyDefault, stream));
		//this->num = other.num;
	}
};

struct SPRING {
	double* k; // spring constant (N/m)
	double* rest; // spring rest length (meters)
	double* damping; // damping on the masses.
	int* left; // index of left mass
	int* right; // index of right mass
	int num;
	SPRING() { num = 0; }
	SPRING(int num, bool on_host = true) {
		cudaError_t(*malloc)(void**, size_t);
		if (on_host) { malloc = &cudaMallocHost; }// if malloc = cudaMallocHost: allocate on host
		else { malloc = &cudaMalloc; }// if malloc = cudaMalloc: allocate on device		
		// ref: https://www.cprogramming.com/tutorial/function-pointers.html
		gpuErrchk((*malloc)((void**)&k, num * sizeof(double)));
		gpuErrchk((*malloc)((void**)&rest, num * sizeof(double)));
		gpuErrchk((*malloc)((void**)&damping, num * sizeof(double)));
		gpuErrchk((*malloc)((void**)&left, num * sizeof(int)));
		gpuErrchk((*malloc)((void**)&right, num * sizeof(int)));
		this->num = num;
	}
	void copyFrom(SPRING& other, cudaStream_t stream = (cudaStream_t)0) { // assuming we have enough streams
		gpuErrchk(cudaMemcpyAsync(k, other.k, num * sizeof(double), cudaMemcpyDefault,stream));
		gpuErrchk(cudaMemcpyAsync(rest, other.rest, num * sizeof(double), cudaMemcpyDefault,stream));
		gpuErrchk(cudaMemcpyAsync(damping, other.damping, num * sizeof(double), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(left, other.left, num * sizeof(int), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(right, other.right, num * sizeof(int), cudaMemcpyDefault, stream));
		//this->num = other.num;
	}
};



struct Joint {
	int* left; // index of left mass
	int* right; // index of right mass
	int* anchor; // anchor_left = anchor[0], anchor_right = anchor[1], 2 number
	int num_left;
	int num_right;
	Joint(){}
	Joint(int num_left,int num_right, bool on_host=true) {
		init(num_left, num_right, on_host);
	}
	void init(int num_left, int num_right, bool on_host = true) {
		cudaError_t(*malloc)(void**, size_t);
		if (on_host) { malloc = &cudaMallocHost; }// if malloc = cudaMallocHost: allocate on host
		else { malloc = &cudaMalloc; }// if malloc = cudaMalloc: allocate on device		
		// ref: https://www.cprogramming.com/tutorial/function-pointers.html
		gpuErrchk((*malloc)((void**)&left, num_left * sizeof(int)));
		gpuErrchk((*malloc)((void**)&right, num_right * sizeof(int)));
		gpuErrchk((*malloc)((void**)&anchor, 2 * sizeof(int)));
		this->num_left = num_left;
		this->num_right = num_right;
	}
	void copyFrom(Joint& other, cudaStream_t stream = (cudaStream_t)0) { // assuming we have enough streams
		gpuErrchk(cudaMemcpyAsync(left, other.left, num_left * sizeof(int), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(right, other.right, num_right * sizeof(int), cudaMemcpyDefault, stream));
		gpuErrchk(cudaMemcpyAsync(anchor, other.anchor, 2 * sizeof(int), cudaMemcpyDefault, stream));
	}

};

class Simulation {
public:
	double dt = 0.001;
	double T = 0; //simulation time
	Vec global_acc = Vec(0,0,0); // global acceleration

	static const int num_joint = 4; //todo:make it dynamic
	// host
	MASS mass;
	SPRING spring;
	Joint joint[num_joint];
	// device
	MASS d_mass;
	SPRING d_spring;
	Joint d_joint[num_joint];
	//size_t num_mass=0;// refer to mass.num
	//size_t num_spring=0;//refer to spring.num

	// control
	bool RUNNING = false;
	bool STARTED = false;
	bool ENDED = false;

	bool FREED = false;
	bool GPU_DONE = false;

	cudaStream_t stream[NUM_CUDA_STREAM]; // cuda stream:https://docs.nvidia.com/cuda/cuda-c-programming-guide/index.html#asynchronous-concurrent-execution

	Simulation() {
		for (int i = 0; i < NUM_CUDA_STREAM; ++i)
			cudaStreamCreate(&stream[i]);// create extra cuda stream: https://docs.nvidia.com/cuda/cuda-c-programming-guide/index.html#asynchronous-concurrent-execution
	}

	Simulation(int num_mass, int num_spring):Simulation() {
		mass = MASS(num_mass, true); // allocate host
		d_mass = MASS(num_mass, false); // allocate device
		spring = SPRING(num_spring, true); // allocate host
		d_spring = SPRING(num_spring, false); // allocate device
		//this->num_mass = num_mass;//refer to spring.num
		//this->num_spring = num_spring;// refer to mass.num
		cudaDeviceSynchronize();
	}
	~Simulation();

	void getAll() {
		mass.copyFrom(d_mass, stream[NUM_CUDA_STREAM-1]);
		spring.copyFrom(d_spring,stream[NUM_CUDA_STREAM - 1]);
		cudaDeviceSynchronize();
	}
	void setAll() {
		d_mass.copyFrom(mass, stream[NUM_CUDA_STREAM - 1]);
		d_spring.copyFrom(spring, stream[NUM_CUDA_STREAM - 1]);
		cudaDeviceSynchronize();
	}

	// Global constraints (can be rendered)
	// creates half-space ax + by + cz < d
	void createPlane(const Vec& abc, const double d, const double FRICTION_K = 0, const double FRICTION_S = 0);
	void createBall(const Vec& center, const double r); // creates ball with radius r at position center
	void clearConstraints(); // clears global constraints only

	void setBreakpoint(const double time); // tell the program to stop at a fixed time (doesn't hang).

	void start();

	void pause(const double t);//pause the simulation at (simulation) time t [s]


	void resume();

	void _run();
	void execute(); // same as above but w/out reset

#ifdef GRAPHICS
	void setViewport(const Vec& camera_position, const Vec& target_location, const Vec& up_vector);
	void moveViewport(const Vec& displacement);
#endif

private:
	

	void waitForEvent();
	void freeGPU();
	inline void updateCudaParameters();


	std::thread gpu_thread;
	std::set<double> bpts; // list of breakpoints
	int massBlocksPerGrid;
	int springBlocksPerGrid;

	std::vector<Constraint*> constraints;
	thrust::device_vector<CudaContactPlane> d_planes; // used for constraints
	thrust::device_vector<CudaBall> d_balls; // used for constraints

	CUDA_GLOBAL_CONSTRAINTS d_constraints;
	bool update_constraints = true;

#ifdef GRAPHICS
	int lineWidth = 2;
	int pointSize = 3;

	GLFWwindow* window;
	int window_width, window_height; // the width and height of the window
	GLuint VertexArrayID; // handle for the vertex array object
	GLuint programID;  // handle for the shader program
	GLuint MatrixID; // handel for the uniform MVP

	glm::mat4 MVP; //model-view-projection matrix
	glm::mat4 View; //view matrix
	glm::mat4 Projection; //projection matrix

	// for projection matrix 
	Vec camera_pos;
	Vec looks_at;
	Vec up;

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
#endif
};





#ifdef GRAPHICS
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
GLFWwindow* createGLFWWindow();

#endif



#endif //TITAN_SIM_H