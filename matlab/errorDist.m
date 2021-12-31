clc;
clear;
close all
folder = "/media/haisenberg/BIGLUCK/Datasets/NCLT/datasets/fastlio_mapping";
date = "2012-01-15";
logFilePath = folder+"/"+date+"/map_pcd/mappingError.txt";
poseFilePath = folder+"/"+date+"/map_pcd/path_mapping.txt";
gtFilePath = "/media/haisenberg/BIGLUCK/Datasets/NCLT/datasets/"+date+"/groundtruth_"+date+".csv";

%% log file reading
fID = fopen(logFilePath);
strPattern = "";
n = 11;
for i=1:n
    strPattern = strPattern+"%f";
end
logData = textscan(fID,strPattern);
timeLog = logData{1}-logData{1}(1);
regiError = logData{5};
inlierRatio2 = logData{4};
inlierRatio = logData{3};
isTMM = logData{2};

%% pose file reading
fID2 = fopen(poseFilePath);
strPattern = "";
n = 7;
for i=1:n
    strPattern = strPattern+"%f";
end
poseData = textscan(fID2,strPattern);
lenPose = length(poseData{1});
matPose = zeros(lenPose,7);
for i=1:lenPose
    for j=1:7
        matPose(i,j) = poseData{j}(i);
    end
end

%% gt reading
% readcsv readmatrix:sth is wrong
fID3 = fopen(gtFilePath);
gtData = textscan(fID3, "%f%s%f%s%f%s%f%s%f%s%f%s%f");
% downsample
downsample = 10;
lenGT = length(gtData{1});
matGT = zeros(floor(lenGT/10),7);
for i=1:floor(lenGT/10)
    for j=1:7
        matGT(i,j) = gtData{2*j-1}(10*i);
    end
end

%% sync with time
timeGT = matGT(:,1)/1e+6; % us -> sec
timePose =  matPose(:,1)/1e+6;
MDtimeGT = KDTreeSearcher(timeGT);
[idx, D] = rangesearch(MDtimeGT,timePose,0.05);
ateError = zeros(lenPose,1);
not_found = 0;
for i=1:lenPose
    if isempty(idx{i})
        not_found = not_found + 1;
        continue;    
    end
    ateError(i) = norm(matPose(i,2:3)-matGT(idx{i}(1),2:3));
end
idxOver1m = find(ateError> 1.0);
%% PLOT
figure(1)
plot(timeLog,isTMM);
hold on
plot(timeLog,inlierRatio2);
plot(timeLog,inlierRatio);
plot(timeLog,regiError);
plot(timePose-timePose(1),ateError);
xlabel("Time (sec)");
ylabel("Absolute trajectory error (m)");
saveas(1,date + "ate_error.jpg");


figure(2)
plot(matPose(:,2),matPose(:,3),".");
hold on
plot(matGT(:,2),matGT(:,3));
plot(matPose(idxOver1m,2),matPose(idxOver1m,3),"*");


% a=[timePose-timePose(1) ateError];
