import gym
import subprocess
import time
import numpy as np
from collections import defaultdict
import msgpack
import socket
import warnings

class FlexipodEnv(gym.Env):
    """
    openai gym compatible environment for the simulated 12DOF flexipod
    
    ref: https://github.com/openai/gym/blob/master/gym/core.py
    """
    # name of command message
    CMD_NAME = ("header", "t", "cmd")

    # name of the returned message
    REC_NAME = ("header",
                "t",           # simulation time [s]
                "joint_pos",   # joint angle [rad]
                "joint_vel",   # joint velocity [rad/s]
                "actuation",   # joint actuation [-1,1]
                "orientation", # base link (body) orientation
                "ang_vel",     # base link (body) angular velocity [rad/s]
                "com_acc",     # base link (body) acceleration
                "com_vel",     # base link (body) velocity
                "com_pos"      # base link (body) position
                )

    ID =defaultdict(None,{name: k  for k,name in enumerate(REC_NAME)})
    CMD_ID = defaultdict(None,{name: k  for k,name in enumerate(CMD_NAME)})
    COMBINED_NAME = tuple(list(REC_NAME)[1:]+["cmd"])
    
    UDP_TERMINATE = -1
    UDP_PAUSE = 17
    UDP_RESUME = 16
    UDP_RESET = 15
    UDP_ROBOT_STATE_REPORT = 14
    UDP_MOTOR_VEL_COMMEND = 13
    UDP_STEP_MOTOR_VEL_COMMEND = 12
    UDP_MOTOR_POS_COMMEND = 11
    UDP_STEP_MOTOR_POS_COMMEND = 10
    
    def __init__(self, dof = 12,
           ip_local = "127.0.0.1", port_local = 32000,
           ip_remote = "127.0.0.1",port_remote = 32001):
        
        super(FlexipodEnv,self).__init__()
        self.local_address = (ip_local, port_local)
        self.remote_address = (ip_remote, port_remote)
        self.packer = msgpack.Packer(use_single_float=True, use_bin_type=True)
        
        self.dof = dof # num joints
        
        # joint_pos,joint_vel,actuation,orientation,ang_vel,com_acc,com_vel,com_pos.z
        state_size = self.dof * 3 + 6 + 3*3+1
        # state = np.empty(state_size,dtype=np.float32)
        action_size = self.dof
        
#         action_size = 6
        # 1,1,0,0,1,1,1,1,0,0,0,0
        
        self.max_action = 1.
        self.action_space = gym.spaces.Box(
            low= -self.max_action*np.ones(action_size,dtype=np.float32),
            high = self.max_action*np.ones(action_size,dtype=np.float32),
            dtype=np.float32)
        
        self.max_observation = np.hstack([
            np.ones(dof)*np.pi,  # joint_pos,
            np.ones(dof)*10.,   # joint_vel
            np.ones(dof),       # actuation
            np.ones(6),         # orientation
            np.ones(3)*30,      # ang_vel
            np.ones(3)*30,      # com_acc
            np.ones(3)*2,       # com_vel
            np.ones(1),         # com_pos_z
        ]).astype(np.float32)
        self.observation_space = gym.spaces.Box(
            low =-self.max_observation,
            high=self.max_observation,
#             shape = state_size,
            dtype = np.float32)

        # reset data packed # https://msgpack-python.readthedocs.io/en/latest/api.html#msgpack.Packer
        self.reset_cmd_b = self.packer.pack([self.UDP_RESET,0,[0,0,0,0]])
        self.pause_cmd_b = self.packer.pack([self.UDP_PAUSE,0,[0,0,0,0]])
        self.resume_cmd_b = self.packer.pack([self.UDP_RESUME,0,[0,0,0,0]])
        self.close_cmd_b = self.packer.pack([self.UDP_TERMINATE,0,[0,0,0,0]])
        self.BUFFER_LEN = 512  # in bytes
        self.TIMEOUT = 0.1 #timeout duration
        
#         self.startSimulation()
#         gc.collect()
        self.send_sock = socket.socket(socket.AF_INET,socket.SOCK_DGRAM) # UDP
        self.send_sock.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1) #immediate reuse of IP address

        
    def __del__(self): # Deleting (Calling destructor) 
        print('Destructor called, FlexipodEnv deleted.')
#         self.close()
    
    def step(self,action = None):
#         step_cmd_b = self.packer.pack([self.UDP_STEP_MOTOR_VEL_COMMEND,time.time(),action])
        if action is not None:
            # 1,1,0,0,1,1,1,1,0,0,0,0
#             alpha = np.pi*0.14
#             beta = np.pi*0.1
#             cmd_action = [
#                 action[0],  # 0
#                 action[1],  # 1
#                 0,          # 2 
#                 0,          # 3
#                 action[2],  # 4  
#                 action[3],  # 5 
#                 action[4],  # 6 
#                 action[5],  # 7 
#                 0,          # 8 
#                 0,          # 9 
#                 0,          # 10
#                 0           # 11
#             ]
            cmd_action = action if type(action) is list else action.tolist()
            step_cmd_b = self.packer.pack([self.UDP_MOTOR_VEL_COMMEND,time.time(),cmd_action])
#             step_cmd_b = self.packer.pack([self.UDP_STEP_MOTOR_VEL_COMMEND,time.time(),cmd_action])
            num_bytes_send = self.send_sock.sendto(step_cmd_b,self.remote_address)
        for k in range(3): # try 3 times
            try:
                msg_rec = self.receive()
                return self._processRecMsg(msg_rec)
            except Exception as e: # raise the exception at the last time
                warnings.warn(f"step(): try #{k}:{e}")
                if k==2: raise e 

    def _processRecMsg(self,msg_rec):
        """processed received message to state action pair"""
        # joint_pos,joint_vel,actuation,orientation,ang_vel,com_acc,com_vel,com_pos.z
        observation = np.hstack(msg_rec[2:-1]+[msg_rec[-1][-1]]).astype(np.float32)
        orientation_z = msg_rec[self.ID['orientation']][2]
        com_vel = np.linalg.norm(msg_rec[self.ID['com_vel']])
        com_z = msg_rec[self.ID['com_pos']][2]
#         print(orientation_z,com_z)
        reward = orientation_z-0.8 + 2.*(msg_rec[self.ID['com_pos']][2]-0.3)-0.2*min(1.0,com_vel)
        
#         reward = orientation_z
        done = True if (orientation_z<0.8)or(com_z<0.3) else False
#         if done:
# #             reward = -10.0
#             print(orientation_z,com_z)
        return observation,reward,done, None
    
    def reset(self):
        for k in range(3):# try 3 times
            try:
                self.send_sock.sendto(self.reset_cmd_b,self.remote_address)
                time.sleep(1e-5)
                msg_rec = self.receive()
                return self._processRecMsg(msg_rec)[0]
            except Exception as e:
                warnings.warn(f"reset(): try #{k}:{e}")
                if k==2: # failed at last time
                    raise e
    
    def render(self,mode="human"):
        pass

    def startSimulation(self):
        try: # check if the simulation is opened
            msg_rec = self.receive()
        except socket.timeout: # program not opened
            task = subprocess.Popen(["run_flexipod.bat"])
            
    def endSimulation(self):
            self.send_sock.sendto(self.close_cmd_b,self.remote_address)

    def receive(self):
        try:
            sock = socket.socket(socket.AF_INET,socket.SOCK_DGRAM) # UDP
#             sock.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1) #immediate reuse of IP address
            sock.settimeout(self.TIMEOUT)
            sock.bind(self.local_address) #Bind the socket to the port
            
            data = sock.recv(self.BUFFER_LEN)
            msg_rec = msgpack.unpackb(data)
            
             # closing connection
            sock.shutdown(socket.SHUT_RDWR)
            sock.close()
            
            data_unpacked = msgpack.unpackb(data)
            return data_unpacked
        except Exception as e:
            sock.shutdown(socket.SHUT_RDWR)
            sock.close()
            raise e
        
    def item_to_String(key,value):
        try:
            return f"{key:<12s}:{','.join(f'{k:>+6.2f}' for k in value)}\n"
        except:
            return f"{key:<12s}:{value:>+6.2f}\n"