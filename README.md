```mermaid
flowchart
    classDef tool fill:#f9f,stroke:#333,stroke-width:2px,color:black;
    classDef file fill:#ff9,stroke:#333,stroke-width:1px,color:black;
    classDef nss fill:#f00,stroke:#333,stroke-width:4px,color:white,font-weight:bold;
    classDef enda fill:#f00AAAAA,stroke:#333,stroke-width:4px
    classDef empty fill:none,stroke:none,color:#0000000;
    subgraph step1["<div style='min-width: 250px'>Step 1:Preparation (Synthesis)</div>"]
        direction TB
        A[Start] ==> B[Synthesis<br>slotEXporter]:::tool
        B --> C[("slotdata-export.txt")]:::file
    end
    subgraph Step2["<div style='min-width: 250px'>Step 2:Editing (Nif Slot Sniper)</div>"]
        direction TB
        C ==> NSS(((Nif Slot Sniper))):::nss
        NifIn[".nif (_0, _1)"]:::file -. "Auto load .nif pair from slotdata-*.txt" .-> NSS
        OspIn[".osp (BodySlide)"]:::file -."Auto load .nif form .osp".-> NSS
        NSS <==> D["slotdata-output.txt <br> (overwrite/slotdataTXT)"]:::file
        NSS --> NifOut["Changed .nif<br>(_0,_1)"]:::file
        NSS -.-> OspOut["Changed .nif<br>(for BodySlide)"]:::file
    end

    subgraph Step3["<div style='min-width: 250px'>Step 3: Finalize&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp&nbsp</div>"]
        direction TB
        NifOut ==> NifFold[Place it in the right place]:::anda
        D ==> E[Synthesis<br>slotIMporter]:::tool
        E --> F["Created .esp<br>(overwrite/GeneratedReplacers)<br>Place it in the right place"]:::anda
        
        OspOut -.-> BS[Run BodySlide]:::tool
    end
Mo2[This tool is designed to run on ModOrganizer2.]
    linkStyle default stroke-width:2px,fill:none,stroke:#333
```
