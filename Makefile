all: mariadb

mariadb:
	g++ -I/opt/halon/include/ -I/usr/include/mariadb/ -I/usr/local/include/ -fPIC -shared mariadb.cpp -lmariadbclient -o mariadb.so
