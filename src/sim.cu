#define GLM_FORCE_PURE
#include "sim.h"


#include <cuda_runtime.h>
#include <cuda.h>
#include <cuda_device_runtime_api.h>
#include <cuda_gl_interop.h>
#include <exception>
#include <device_launch_parameters.h>



__global__ void computeSpringForces(MASS d_mass,SPRING d_spring,const int num_spring) {
	for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < num_spring; i += blockDim.x * gridDim.x) {
		int right = d_spring.right[i];
		int left = d_spring.left[i];

		Vec s_vec = d_mass.pos[right] - d_mass.pos[left];// the vector from left to right
		double length = s_vec.norm(); // current spring length
		s_vec /= length; // normalized to unit vector (direction) //Todo: instablility for small length
		Vec force = d_spring.k[i] * (d_spring.rest[i] - length) * s_vec; // normal spring force
		force += dot(d_mass.vel[left] - d_mass.vel[right], s_vec) * d_spring.damping[i] * s_vec;// damping

		if (d_mass.fixed[right] == false) {
			d_mass.force[right].atomicVecAdd(force); // need atomics here
		}
		if (d_mass.fixed[left] == false) {
			d_mass.force[left].atomicVecAdd(-force);
		}
	}
}



__global__ void computeSpringForces(
	const Vec* __restrict__ mass_pos,
	const Vec* __restrict__ mass_vel, 
	Vec* mass_force, 
	const bool* __restrict__ mass_fixed,
	const double* __restrict__ spring_k, 
	const double* __restrict__ spring_rest, 
	const double* __restrict__ spring_damping,
	const int* __restrict__ spring_left,
	const int* __restrict__ spring_right, const int num_spring) {
	for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < num_spring; i += blockDim.x * gridDim.x) {

		int right = spring_right[i];
		int left = spring_left[i];

		Vec s_vec = mass_pos[right] - mass_pos[left];// the vector from left to right
		double length = s_vec.norm(); // current spring length
		s_vec /= length; // normalized to unit vector (direction) //Todo: instablility for small length
		Vec force = spring_k[i] * (spring_rest[i] - length) * s_vec; // normal spring force
		force += dot(mass_vel[left] - mass_vel[right], s_vec) * spring_damping[i] * s_vec;// damping

		if (mass_fixed[right] == false) {
			mass_force[right].atomicVecAdd(force); // need atomics here
		}
		if (mass_fixed[left] == false) {
			mass_force[left].atomicVecAdd(-force);
		}
	}
}

__global__ void massForcesAndUpdate(MASS d_mass, const int num_mass,
	const Vec global_acc, const CUDA_GLOBAL_CONSTRAINTS c,const double dt) {
	for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < num_mass; i += blockDim.x * gridDim.x) {
		if (d_mass.fixed[i] == false) {

			Vec force = global_acc;
			force *= d_mass.m[i]; // force = d_mass.m[i] * global_acc;
			force += d_mass.force[i];
			force += d_mass.force_extern[i];// add external force [N]

			for (int j = 0; j < c.num_planes; j++) { // global constraints
				c.d_planes[j].applyForce(force, d_mass.pos[i], d_mass.vel[i]); // todo fix this 
			}
			for (int j = 0; j < c.num_balls; j++) {
				c.d_balls[j].applyForce(force, d_mass.pos[i]);
			}
			d_mass.acc[i] = force / d_mass.m[i];
			d_mass.vel[i] += d_mass.acc[i] * dt;
			d_mass.pos[i] += d_mass.vel[i] * dt;
			d_mass.force[i].setZero();
		}
	}
}

__global__ void massForcesAndUpdate(
	const double* __restrict__ mass_m,
	Vec* mass_pos,
	Vec* mass_vel,
	Vec* mass_acc,
	Vec* mass_force,
	const Vec* __restrict__ mass_force_extern,
	const bool* __restrict__ mass_fixed,
	const int num_mass,
	const Vec global_acc, 
	const CUDA_GLOBAL_CONSTRAINTS c, 
	const double dt) {
	for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < num_mass; i += blockDim.x * gridDim.x) {
		if (mass_fixed[i] == false) {
			Vec force = global_acc;
			force *= mass_m[i]; // force = d_mass.m[i] * global_acc;
			force += mass_force[i];
			force += mass_force_extern[i];// add external force [N]

			for (int j = 0; j < c.num_planes; j++) { // global constraints
				c.d_planes[j].applyForce(force, mass_pos[i], mass_vel[i]); // todo fix this 
			}
			for (int j = 0; j < c.num_balls; j++) {
				c.d_balls[j].applyForce(force, mass_pos[i]);
			}
			mass_acc[i] = force / mass_m[i];
			mass_vel[i] += mass_acc[i] * dt;
			mass_pos[i] += mass_vel[i] * dt;
			mass_force[i].setZero();
		}
	}
}

//__global__ void dynamicsUpdate(MASS d_mass, SPRING d_spring,const int num_mass, const int num_spring, 
//								const Vec global_acc, const CUDA_GLOBAL_CONSTRAINTS d_constraints, const double dt,
//								int massBlocksPerGrid,int springBlocksPerGrid) {
//	for (int i = 0; i < NUM_QUEUED_KERNELS; i++) {
//
//		computeSpringForces << <springBlocksPerGrid, THREADS_PER_BLOCK >> > (d_mass.pos, d_mass.vel, d_mass.force, d_mass.fixed,
//			d_spring.k, d_spring.rest, d_spring.damping, d_spring.left, d_spring.right, num_spring);
//
//		massForcesAndUpdate << <massBlocksPerGrid, THREADS_PER_BLOCK >> > (d_mass.m, d_mass.pos, d_mass.vel, d_mass.acc,
//			d_mass.force, d_mass.force_extern, d_mass.fixed, num_mass, global_acc,
//			d_constraints, dt);
//	}
//}

inline void Simulation::updateCudaParameters() {
	massBlocksPerGrid = (num_mass + THREADS_PER_BLOCK - 1) / THREADS_PER_BLOCK;
	springBlocksPerGrid = (num_spring + THREADS_PER_BLOCK - 1) / THREADS_PER_BLOCK;
	if (massBlocksPerGrid > MAX_BLOCKS) { massBlocksPerGrid = MAX_BLOCKS; }
	if (springBlocksPerGrid > MAX_BLOCKS) { springBlocksPerGrid = MAX_BLOCKS; }
}

void Simulation::setBreakpoint(const double time) {
	if (ENDED) { throw std::runtime_error("Simulation has ended. Can't modify simulation after simulation end."); }
	bpts.insert(time); // TODO mutex breakpoints
}

/*pause the simulation at (simulation) time t [s] */
void Simulation::pause(const double t) {
	if (ENDED && !FREED) { throw std::runtime_error("Simulation has ended. can't call control functions."); }
	setBreakpoint(t);
	waitForEvent();
}

void Simulation::start() {
	if (ENDED) { throw std::runtime_error("The simulation has ended. Cannot call sim.start() after the end of the simulation."); }
	if (num_mass == 0) { throw std::runtime_error("No masses have been added. Please add masses before starting the simulation."); }
	printf("Starting simulation with %d masses and %d springs\n", num_mass, num_spring);
	RUNNING = true;
	STARTED = true;

	T = 0;

	if (this->dt == 0.0) { // if dt hasn't been set by the user.
		dt = 0.01; // min delta
	}
	updateCudaParameters();

	d_constraints.d_balls = thrust::raw_pointer_cast(&d_balls[0]);
	d_constraints.d_planes = thrust::raw_pointer_cast(&d_planes[0]);
	d_constraints.num_balls = d_balls.size();
	d_constraints.num_planes = d_planes.size();

	update_constraints = false;

	setAll();// copy mass and spring to gpu

	gpu_thread = std::thread(&Simulation::_run, this); //TODO: thread
}

void Simulation::_run() { // repeatedly start next
#ifdef GRAPHICS
	window = createGLFWWindow();

	glGenVertexArrays(1, &VertexArrayID);//GLuint VertexArrayID;
	glBindVertexArray(VertexArrayID);

	// Create and compile our GLSL program from the shaders
	this->programID = LoadShaders(); // ("shaders/StandardShading.vertexshader", "shaders/StandardShading.fragmentshader"); //
	// Get a handle for our "MVP" uniform
	computeMVP(); // compute perspective projection matrix
	this->MatrixID = glGetUniformLocation(programID, "MVP"); // doesn't seem to be necessary
	generateBuffers(); // generate buffers for all masses and springs

	for (Constraint* c : constraints) { // generate buffers for constraint objects
		c->generateBuffers();
	}

	glUseProgram(programID);// Use our shader

	updateBuffers();//Todo might not need?
#endif
	execute();
	GPU_DONE = true;
}


void Simulation::execute() {

	auto start = std::chrono::steady_clock::now();
	while (true) {
		if (!bpts.empty() && *bpts.begin() <= T) {
			cudaDeviceSynchronize(); // synchronize before updating the springs and mass positions
		//            std::cout << "Breakpoint set for time " << *bpts.begin() << " reached at simulation time " << T << "!" << std::endl;
			bpts.erase(bpts.begin());
			RUNNING = false;
			while (!RUNNING) {
				std::this_thread::sleep_for(std::chrono::nanoseconds(100));
				if (ENDED) {

					auto end = std::chrono::steady_clock::now();
					printf("Elapsed time:%d ms \n", 
						std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

					//for (Constraint* c : constraints) {
					//	delete c;
					//}
#ifdef GRAPHICS
					glDeleteBuffers(1, &vertexbuffer);
					glDeleteBuffers(1, &colorbuffer);
					glDeleteBuffers(1, &elementbuffer);
					glDeleteProgram(programID);
					glDeleteVertexArrays(1, &VertexArrayID);
					glfwTerminate(); // Close OpenGL window and terminate GLFW
#endif
					return;
				}
			}

			if (resize_buffers) {
				resizeBuffers(); // needs to be run from GPU thread
				resize_buffers = false;
				update_colors = true;
				update_indices = true;
			}

			if (update_constraints) {
				d_constraints.d_balls = thrust::raw_pointer_cast(&d_balls[0]);
				d_constraints.d_planes = thrust::raw_pointer_cast(&d_planes[0]);
				d_constraints.num_balls = d_balls.size();
				d_constraints.num_planes = d_planes.size();

				for (Constraint* c : constraints) { // generate buffers for constraint objects
					if (!c->_initialized)
						c->generateBuffers();
				}
				update_constraints = false;
			}
			continue;
		}

		//dynamicsUpdate << <1, 1 >> > (d_mass, d_spring, num_mass, num_spring, global_acc, d_constraints, dt, massBlocksPerGrid, springBlocksPerGrid);
		
		for (int i = 0; i < NUM_QUEUED_KERNELS; i++) {
			//computeSpringForces << <springBlocksPerGrid, THREADS_PER_BLOCK >> > (d_mass, d_spring, num_spring); // compute mass forces after syncing
			
			computeSpringForces << <springBlocksPerGrid, THREADS_PER_BLOCK >> > (d_mass.pos, d_mass.vel, d_mass.force, d_mass.fixed,
				d_spring.k, d_spring.rest, d_spring.damping, d_spring.left, d_spring.right, num_spring);
			//gpuErrchk(cudaPeekAtLastError());

			//massForcesAndUpdate << <massBlocksPerGrid, THREADS_PER_BLOCK >> > (d_mass, num_mass, global_acc, d_constraints, dt);
			
			
			massForcesAndUpdate << <massBlocksPerGrid, THREADS_PER_BLOCK >> > (d_mass.m,d_mass.pos,d_mass.vel,d_mass.acc,
				d_mass.force,d_mass.force_extern,d_mass.fixed,num_mass,global_acc,
				d_constraints,dt);
			//gpuErrchk(cudaPeekAtLastError());

		}
		T += NUM_QUEUED_KERNELS * dt;

#ifdef GRAPHICS
		if (fmod(T, 1./60.) < NUM_QUEUED_KERNELS * dt) {
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); // clear screen
			glUniformMatrix4fv(MatrixID, 1, GL_FALSE, &MVP[0][0]);// update transformation "MVP" uniform

			computeMVP(false); // update MVP, dont update camera matrix //todo

			for (Constraint* c : constraints) {
				c->draw();
			}

			//updateBuffers();
			updateVertexBuffers();

			//cudaDeviceSynchronize(); // synchronize before updating the springs and mass positions

			draw();

			// Swap buffers, render screen
			glfwPollEvents();
			glfwSwapBuffers(window);

			if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS || glfwWindowShouldClose(window) != 0) {
				exit(1); // TODO maybe deal with memory leak here.
			}
		}
#endif
	}


}


void Simulation::resume() {
	if (ENDED) { throw std::runtime_error("Simulation has ended. Cannot resume simulation."); }
	if (!STARTED) { throw std::runtime_error("Simulation has not started. Cannot resume before calling sim.start()."); }
	if (num_mass == 0) { throw std::runtime_error("No masses have been added. Add masses before simulation starts."); }
	updateCudaParameters();
	cudaDeviceSynchronize();
	RUNNING = true;
}

Simulation::~Simulation() {
	std::cerr << "Simulation destructor called." << std::endl;

	if (STARTED) {
		waitForEvent();

		ENDED = true; // TODO maybe race condition

		while (!GPU_DONE) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));// TODO fix race condition
		}
		if (gpu_thread.joinable()) {
			gpu_thread.join();
		}
		else {
			printf("could not join GPU thread.\n");
			exit(1);
		}
	}
	if (!FREED) {
		freeGPU();
	}
}

void Simulation::waitForEvent() {
	if (ENDED && !FREED) { throw std::runtime_error("Simulation has ended. can't call control functions."); }
	while (RUNNING) {
		std::this_thread::sleep_for(std::chrono::nanoseconds(100));
	}
}

void Simulation::freeGPU() {
	FREED = true;
	ENDED = true; // just to be safe
	//Todo
	//for (Spring* s : springs) {
	//	s->_left = nullptr;
	//	s->_right = nullptr;
	//	delete s;
	//}
	//for (Mass* m : masses) {
	//	if (m->arrayptr) {
	//		gpuErrchk(cudaFree(m->arrayptr));
	//	}
	//	delete m;
	//}
	d_balls.clear();
	d_balls.shrink_to_fit();

	d_planes.clear();
	d_planes.shrink_to_fit();
	printf("GPU freed\n");
}


// creates half-space ax + by + cz < d
void Simulation::createPlane(const Vec& abc, const double d, const double FRICTION_K, const double FRICTION_S) { // creates half-space ax + by + cz < d
	if (ENDED) { throw std::runtime_error("The simulation has ended. New objects cannot be created."); }
	ContactPlane* new_plane = new ContactPlane(abc, d);
	new_plane->_FRICTION_K = FRICTION_K;
	new_plane->_FRICTION_S = FRICTION_S;
	constraints.push_back(new_plane);

	CudaContactPlane cuda_contact_plane;
	cuda_contact_plane._normal = new_plane->_normal;
	cuda_contact_plane._offset = d;
	cuda_contact_plane._FRICTION_K = FRICTION_K;
	cuda_contact_plane._FRICTION_S = FRICTION_S;

	d_planes.push_back(cuda_contact_plane);


	//d_planes.push_back(CudaContactPlane(*new_plane));
	update_constraints = true;
}

void Simulation::createBall(const Vec& center, const double r) { // creates ball with radius r at position center
	if (ENDED) { throw std::runtime_error("The simulation has ended. New constraints cannot be added."); }
	Ball* new_ball = new Ball(center, r);
	constraints.push_back(new_ball);
	
	CudaBall cuda_ball;
	cuda_ball._center = center;
	cuda_ball._radius = r;
	d_balls.push_back(cuda_ball);

	//d_balls.push_back(CudaBall(*new_ball));
	update_constraints = true;
}

void Simulation::clearConstraints() { // clears global constraints only
	constraints.clear();
	update_constraints = true;
}

#ifdef GRAPHICS
void Simulation::setViewport(const Vec& camera_position, const Vec& target_location, const Vec& up_vector) {
	this->camera_pos = camera_position;
	this->looks_at = target_location;
	this->up = up_vector;
	if (STARTED) { computeMVP(); }
}
void Simulation::moveViewport(const Vec& displacement) {
	this->camera_pos += displacement;
	if (STARTED) { computeMVP(); } // compute perspective projection matrix
}
void Simulation::computeMVP(bool update_view) {
	// http://www.opengl-tutorial.org/beginners-tutorials/tutorial-3-matrices/#cumulating-transformations--the-modelviewprojection-matrix
	int width, height;
	glfwGetFramebufferSize(window, &width, &height); // check if window is resized
	bool is_resized = width != window_width || height != window_height;
	if (is_resized) { // window is resized
		glfwGetFramebufferSize(window, &window_width, &window_height);
		// Projection matrix : 60� Field of View, 4:3 ratio, display range : 0.01 unit <-> 100 units
		this->Projection = glm::perspective(glm::radians(60.0f), (float)window_width / (float)window_height, 0.01f, 100.0f);
	}
	if (update_view) {
		// Camera matrix
		this->View = glm::lookAt(
			glm::vec3(camera_pos[0], camera_pos[1], camera_pos[2]), // Camera is at (4,3,3), in World Space
			glm::vec3(looks_at[0], looks_at[1], looks_at[2]), // and looks at the origin
			glm::vec3(up[0], up[1], up[2]));  // Head is up (set to 0,-1,0 to look upside-down)
	}
	if (is_resized || update_view) {
		this->MVP = Projection * View; // Remember, matrix multiplication is the other way around
	}
}

inline void Simulation::generateBuffers() {

		//GLuint colorbuffer; // bind colors to buffer colorbuffer 
		glGenBuffers(1, &colorbuffer);
		glBindBuffer(GL_ARRAY_BUFFER, colorbuffer);
		glBufferData(GL_ARRAY_BUFFER, 3 * num_mass * sizeof(GLfloat), NULL, GL_DYNAMIC_DRAW);
		cudaGLRegisterBufferObject(colorbuffer);
		cudaGLMapBufferObject(&colorPointer, colorbuffer); // refer to updateBuffers()

		//GLuint elementbuffer; // create buffer for main object
		glGenBuffers(1, &elementbuffer);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementbuffer);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, 2 * num_spring * sizeof(GLuint), NULL, GL_DYNAMIC_DRAW); // second argument is number of bytes
		cudaGLRegisterBufferObject(elementbuffer);
		cudaGLMapBufferObject(&indexPointer, elementbuffer);// refer to updateBuffers()

		//GLuint vertexbuffer; // bind vertex buffer
		glGenBuffers(1, &vertexbuffer);
		glBindBuffer(GL_ARRAY_BUFFER, vertexbuffer);
		glBufferData(GL_ARRAY_BUFFER, 3 * num_mass * sizeof(GLfloat), NULL, GL_DYNAMIC_DRAW);
		cudaGLRegisterBufferObject(vertexbuffer);
		cudaGLMapBufferObject(&vertexPointer, vertexbuffer);// refer to updateBuffers()

	//Todo: maybe unbind buffer? see updateBuffers()
}

inline void Simulation::resizeBuffers() {
	//    std::cout << "resizing buffers (" << masses.size() << " masses, " << springs.size() << " springs)." << std::endl;
	//    std::cout << "resizing buffers (" << d_masses.size() << " device masses, " << d_springs.size() << " device springs)." << std::endl;
		cudaGLUnmapBufferObject(colorbuffer);//refer to updateBuffers()
		cudaGLUnregisterBufferObject(this->colorbuffer);
		glBindBuffer(GL_ARRAY_BUFFER, this->colorbuffer);
		glBufferData(GL_ARRAY_BUFFER, 3 * num_mass * sizeof(GLfloat), NULL, GL_DYNAMIC_DRAW);
		cudaGLRegisterBufferObject(this->colorbuffer);
		cudaGLMapBufferObject(&colorPointer, colorbuffer);//refer to updateBuffers()

		cudaGLUnmapBufferObject(elementbuffer);//refer to updateBuffers()
		cudaGLUnregisterBufferObject(this->elementbuffer);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->elementbuffer);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, 2 * num_spring * sizeof(GLuint), NULL, GL_DYNAMIC_DRAW); // second argument is number of bytes
		cudaGLRegisterBufferObject(this->elementbuffer);
		cudaGLMapBufferObject(&indexPointer, elementbuffer);//refer to updateBuffers()

		cudaGLUnmapBufferObject(vertexbuffer);//refer to updateBuffers()
		cudaGLUnregisterBufferObject(this->vertexbuffer);
		glBindBuffer(GL_ARRAY_BUFFER, vertexbuffer);
		glBufferData(GL_ARRAY_BUFFER, 3 * num_mass * sizeof(GLfloat), NULL, GL_DYNAMIC_DRAW);
		cudaGLRegisterBufferObject(this->vertexbuffer);
		cudaGLMapBufferObject(&vertexPointer, vertexbuffer);//refer to updateBuffers()

	resize_buffers = false;
}

__global__ void updateVertices(float* __restrict__ gl_ptr, const Vec* __restrict__  pos, const int num_mass) {
	// https://devblogs.nvidia.com/cuda-pro-tip-write-flexible-kernels-grid-stride-loops/
	// https://devblogs.nvidia.com/cuda-pro-tip-optimize-pointer-aliasing/
	for (int i = blockIdx.x * blockDim.x + threadIdx.x;i < num_mass;i += blockDim.x * gridDim.x){
		gl_ptr[3 * i] = (float)pos[i][0];
		gl_ptr[3 * i + 1] = (float)pos[i][1];
		gl_ptr[3 * i + 2] = (float)pos[i][2];
	}
}

__global__ void updateIndices(unsigned int* __restrict__ gl_ptr, 
							  const int* __restrict__ left,const int* __restrict__ right, int num_spring) {
	for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < num_spring; i += blockDim.x * gridDim.x) {
		gl_ptr[2 * i] = (unsigned int)left[i]; // todo check if this is needed
		gl_ptr[2 * i + 1] = (unsigned int)right[i];
	}
}

__global__ void updateColors(float* __restrict__ gl_ptr, const Vec* __restrict__ color, const int num_mass) {
	for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < num_mass; i += blockDim.x * gridDim.x) {
		gl_ptr[3 * i] = (float)color[i][0];
		gl_ptr[3 * i + 1] = (float)color[i][1];
		gl_ptr[3 * i + 2] = (float)color[i][2];
	}
}

void Simulation::updateBuffers() { // todo: check the kernel call
	if (update_colors) {
		//glBindBuffer(GL_ARRAY_BUFFER, colorbuffer);
		//void* colorPointer; // if no masses, springs, or colors are changed/deleted, this can be start only once
		//cudaGLMapBufferObject(&colorPointer, colorbuffer);
		updateColors<<<massBlocksPerGrid, THREADS_PER_BLOCK,0, stream[0]>>>((float*)colorPointer, d_mass.color, num_mass);
		//cudaGLUnmapBufferObject(colorbuffer);
		update_colors = false;
	}
	if (update_indices) {
		//glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementbuffer);
		//void* indexPointer; // if no masses or springs are deleted, this can be start only once
		//cudaGLMapBufferObject(&indexPointer, elementbuffer);
		updateIndices<<<springBlocksPerGrid, THREADS_PER_BLOCK,0,stream[1]>>>((unsigned int*)indexPointer, d_spring.left,d_spring.right, num_spring);
		//cudaGLUnmapBufferObject(elementbuffer);
		update_indices = false;
	}
	{
		//glBindBuffer(GL_ARRAY_BUFFER, vertexbuffer);
		//void* vertexPointer;
		//cudaGLMapBufferObject(&vertexPointer, vertexbuffer);
		updateVertices<<<massBlocksPerGrid, THREADS_PER_BLOCK>>>((float*)vertexPointer, d_mass.pos, num_mass);
		//cudaGLUnmapBufferObject(vertexbuffer);
	}
}

void Simulation::updateVertexBuffers() {

	updateVertices << <massBlocksPerGrid, THREADS_PER_BLOCK >> > ((float*)vertexPointer, d_mass.pos, num_mass);
	//cudaGLUnmapBufferObject(vertexbuffer);
}

inline void Simulation::draw() {
	glEnableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, this->vertexbuffer);
	glPointSize(this->pointSize);
	glLineWidth(this->lineWidth);
	glVertexAttribPointer(
		0,                  // attribute. No particular reason for 0, but must match the layout in the shader.
		3,                  // size
		GL_FLOAT,           // type
		GL_FALSE,           // normalized?
		0,                  // stride
		(void*)0            // array buffer offset
		);

	glEnableVertexAttribArray(1);
	glBindBuffer(GL_ARRAY_BUFFER, this->colorbuffer);
	glVertexAttribPointer(
		1,                                // attribute. No particular reason for 1, but must match the layout in the shader.
		3,                                // size
		GL_FLOAT,                         // type
		GL_FALSE,                         // normalized?
		0,                                // stride
		(void*)0                          // array buffer offset
		);

	glDrawArrays(GL_POINTS, 0, num_mass); // 3 indices starting at 0 -> 1 triangle
	glDrawElements(GL_LINES, 2 * num_spring, GL_UNSIGNED_INT, (void*)0); // 2 indices for a line

	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);
}


void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
	glViewport(0, 0, width, height);
}
GLFWwindow* createGLFWWindow() {
	// Initialise GLFW
	if (!glfwInit()) { throw(std::runtime_error("Failed to initialize GLFW\n")); }
	////// MSAA: multisampling
	//glfwWindowHint(GLFW_SAMPLES, 0); // #samples to use for multisampling. Zero disables multisampling.
	//glEnable(GL_MULTISAMPLE);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4); // use GLSL 4.6
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // meke opengl forward compatible
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); //We don't want the old OpenGL
	glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);
	glfwSwapInterval(0);// disable vsync
	// Open a window and create its OpenGL context
	GLFWwindow* window = glfwCreateWindow(1920, 1080, "CUDA Physics Simulation", NULL, NULL);
	if (window == NULL) {
		fprintf(stderr,"Failed to open GLFW window. If you have an Intel GPU, they are not 3.3 compatible.\n");
		getchar();
		glfwTerminate();
		exit(1);
	}
	glfwMakeContextCurrent(window);
	glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
	glEnable(GL_DEPTH_TEST);
	//    // Accept fragment if it closer to the camera than the former one
	glDepthFunc(GL_LESS);
	// Initialize GLEW
	glewExperimental = true; // Needed for core profile
	if (glewInit() != GLEW_OK) {
		fprintf(stderr, "Failed to initialize GLEW\n");
		getchar();
		glfwTerminate();
		exit(1);
	}
	// Ensure we can capture the escape key being pressed below
	glfwSetInputMode(window, GLFW_STICKY_KEYS, GL_TRUE);
	// reset window color
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	return window;
}

#endif
