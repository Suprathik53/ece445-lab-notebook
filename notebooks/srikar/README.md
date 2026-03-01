# Srikar Worklog

[[_TOC_]]

# 2026-02-10 - Discussion with TA
This was our first meeting with our TA Zhuchen Shao. We discussed our idea and walked through our initial block diagram with the TA. He then provided some direction on how to go about ordering parts and told us about some other project logistics.

# 2026-02-13 - Finishing project proposal
Throughout the week our group worked on our project proposal. With some research I was able to finalize our parts list for our project. We also finalized our block diagram.

This is our block diagram.
![](block_diagram.png)

I found the MAX4466 Mic Amp which comes with both a microphone and ADC on the board. It has adjustable gain and is low cost making it convenient for us to use.
![](max4466.jpg)
[link](https://www.digikey.com/en/products/detail/adafruit-industries-llc/1063/4990762)

Then for our DAC and speaker amplifier I found the MAX98357A
![](max98357A.jpg)

These parts will be combined with an ESP32, a generic speaker, and some other small parts to create our final prodcut.

# 2026-02-19 - Second Discussion with TA and Schematic
This week we created our schematic to begin our PCB design and reviewed it with our TA. We then started working on the order form for our parts.

Our schematic:
![](schematic.png)

# 2026-02-25 - Finalizing PCB Design
I updated our schematic to match the parts we ordered. I then moved onto routing the PCB. I placed the components such that the user interface is on the front of the pcb while power would be on the back. I had to redo the placement for the most optimal routing. Eventually I was able to get a well organized PCB and I sent it to the TA for ordering. 

This is the final layout:
![](pcb_routing.png)