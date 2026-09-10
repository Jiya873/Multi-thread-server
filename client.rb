

require 'socket'
starttime = Process.clock_gettime(Process::CLOCK_MONOTONIC)    #record the start time of the client program

s = TCPSocket.open('localhost', 8989)    #create a TCP socket and connect to the server at localhost on port 8989

s.write("/tmp/testfiles/#{ARGV[0]}.c\n")    #send the requested file path to the server

s.each_line do |line|    #read the response from the server line by line
  puts line    #print each line of the response to the console
end

s.close    #close the socket connection to the server
endtime = Process.clock_gettime(Process::CLOCK_MONOTONIC)    #record the end time of the client program
elapsed_time = endtime - starttime    #calculate the elapsed time by subtracting the start time from the end time
puts "Elapsed time: #{elapsed_time} (#{ARGV[0]})"    #print the elapsed time to the console