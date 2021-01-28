import can
from can import Message
can_interface = 'can0'
bus = can.interface.Bus(can_interface, bustype='socketcan')
bus.send(Message(data=[0x59,0x45], arbitration_id=0x7E0, is_extended_id=False))
while True:
  message = bus.recv(0.05)
  if (message is not None and message.arbitration_id == 0x7E8):
    print("SUCCESS")
    bus.send(Message(data=[0x6B], arbitration_id=0x7E0, is_extended_id=False))
  if (message is not None and message.arbitration_id == 0x0A7):
    print("FAILURE")
    break
